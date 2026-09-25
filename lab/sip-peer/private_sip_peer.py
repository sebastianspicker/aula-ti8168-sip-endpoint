"""Protocol-neutral, bounded UDP SIP/RTP peer for one authorised LS200 source.

Only counters, timings, tuples, SSRCs, and SHA-256 packet digests survive in
evidence. SIP bodies and RTP payloads are transient parsing inputs.

See `qemu_profile.py` in this directory for the QEMU-profile subclass and its
fixed guest NAT defaults.
"""
from __future__ import annotations

import base64
import hashlib
import ipaddress
import select
import socket
import struct
import threading
import time
from dataclasses import dataclass, field
from typing import Callable

SIP_PORT = 15060
MEDIA_PORT_MIN, MEDIA_PORT_MAX = 15200, 15298
LS200_MEDIA_PORT_MIN, LS200_MEDIA_PORT_MAX = 40000, 40100
_TAG = "ls200-private-peer"
_BASELINE_H264_PROFILE = "42e01f"


class ProtocolRejected(ValueError):
    pass


def _ipv4(value: str, label: str) -> str:
    try:
        address = ipaddress.ip_address(value)
    except ValueError as error:
        raise ValueError(f"{label} must be a specific IPv4 literal") from error
    if not isinstance(address, ipaddress.IPv4Address) or address.is_unspecified or address.is_multicast:
        raise ValueError(f"{label} must be a specific IPv4 literal")
    return str(address)


def _rtp(payload_type: int, sequence: int, timestamp: int, ssrc: int, payload: bytes, marker: bool = False) -> bytes:
    return struct.pack("!BBHII", 0x80, payload_type | (0x80 if marker else 0), sequence, timestamp, ssrc) + payload


@dataclass
class PeerEvidence:
    invite_seen: bool = False
    ack_seen: bool = False
    bye_seen: bool = False
    peer_bye_sent: int = 0
    peer_bye_200_seen: int = 0
    withheld_final_invites: int = 0
    withheld_final_cancels: int = 0
    withheld_final_timeouts: int = 0
    dialogs_started: int = 0
    dialogs_completed: int = 0
    rejected_sip_packets: int = 0
    rejected_media_packets: int = 0
    outbound_video_packets: int = 0
    outbound_audio_packets: int = 0
    outbound_video_rtcp_packets: int = 0
    outbound_audio_rtcp_packets: int = 0
    inbound_video_packets: int = 0
    inbound_audio_packets: int = 0
    dtmf_tone: int | None = None
    dtmf_tones: list[int] = field(default_factory=list)
    sip_source: tuple[str, int] | None = None
    video_source: tuple[str, int] | None = None
    audio_source: tuple[str, int] | None = None
    video_ssrc: int | None = None
    audio_ssrc: int | None = None
    invite_at_seconds: float | None = None
    ack_at_seconds: float | None = None
    bye_at_seconds: float | None = None
    peer_bye_at_seconds: float | None = None
    peer_bye_200_at_seconds: float | None = None
    withheld_final_at_seconds: float | None = None
    outbound_video_sha256: str = ""
    outbound_audio_sha256: str = ""


class SipRtpPeer:
    """One active correlated SIP dialog and RTCP-muxed H264/PCMU/PCMA media."""
    def __init__(self, sip_port: int = SIP_PORT, *, bind_address: str,
                 advertised_address: str, authorized_source_address: str,
                 media_port_min: int = MEDIA_PORT_MIN, media_port_max: int = MEDIA_PORT_MAX,
                 ls200_media_port_min: int = LS200_MEDIA_PORT_MIN,
                 ls200_media_port_max: int = LS200_MEDIA_PORT_MAX,
                 withhold_first_invite_final: bool = False,
                 withheld_final_timeout: float = 5.0,
                 peer_initiated_hangup: bool = False,
                 signaling_only: bool = False,
                 allow_distinct_rtcp_tuple: bool = False) -> None:
        if not 0 <= sip_port <= 65535:
            raise ValueError("SIP port must be between 0 and 65535")
        if media_port_min % 2 or media_port_max % 2 or media_port_min > media_port_max:
            raise ValueError("media pool must be an ascending inclusive even port range")
        if not 1024 <= ls200_media_port_min <= ls200_media_port_max <= 65535:
            raise ValueError("LS200 media ports must be an ascending UDP port range")
        if withhold_first_invite_final and withheld_final_timeout <= 0:
            raise ValueError("withheld final timeout must be positive")
        self.bind_address = _ipv4(bind_address, "SIP peer bind address")
        self.advertised_address = _ipv4(advertised_address, "SIP peer advertised address")
        self.authorized_source_address = _ipv4(authorized_source_address, "authorised LS200 source address")
        self.ls200_media_port_min, self.ls200_media_port_max = ls200_media_port_min, ls200_media_port_max
        self.withhold_first_invite_final = withhold_first_invite_final
        self.withheld_final_timeout = withheld_final_timeout
        self.peer_initiated_hangup = peer_initiated_hangup
        self.signaling_only = signaling_only
        self.allow_distinct_rtcp_tuple = allow_distinct_rtcp_tuple
        self.sip = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sip.bind((self.bind_address, sip_port)); self.sip.setblocking(False)
        self.port = self.sip.getsockname()[1]
        self.video = self._media_socket(media_port_min, media_port_max)
        self.audio = self._media_socket(media_port_min, media_port_max)
        self.evidence = PeerEvidence(); self._started = time.monotonic()
        self._video_hash, self._audio_hash = hashlib.sha256(), hashlib.sha256()
        self._stop = threading.Event(); self._closed = False; self._error: BaseException | None = None
        self._thread = threading.Thread(target=self._run, name="ls200-sip-rtp-peer", daemon=True)
        self._first_invite_final_withheld = False
        self._clear_dialog()

    def _media_socket(self, first: int, last: int) -> socket.socket:
        for port in range(first, last + 1, 2):
            descriptor = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                descriptor.bind((self.bind_address, port)); descriptor.setblocking(False); return descriptor
            except OSError:
                descriptor.close()
        raise OSError("no configured RTP fixture port is available")

    def __enter__(self) -> SipRtpPeer:
        self._thread.start(); return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def close(self) -> None:
        if self._closed: return
        self._closed = True; self._stop.set()
        if self._thread.ident is not None:
            self._thread.join(2.0)
            if self._thread.is_alive(): self._error = RuntimeError("peer thread did not stop within 2 seconds")
        for descriptor in (self.sip, self.video, self.audio): descriptor.close()
        self.evidence.outbound_video_sha256 = self._video_hash.hexdigest()
        self.evidence.outbound_audio_sha256 = self._audio_hash.hexdigest()
        self._clear_dialog()
        if self._error is not None: raise RuntimeError("SIP/RTP peer failed") from self._error

    def assert_healthy(self) -> None:
        if self._error is not None: raise RuntimeError("SIP/RTP peer failed") from self._error

    def _clear_dialog(self) -> None:
        self._sip_guest = self._call_id = self._from = self._to = None
        self._invite_request = None
        self._invite_cseq = None; self._sip_state = "idle"; self._video_guest = self._audio_guest = None
        self._video_ssrc = self._audio_ssrc = self._video_rtcp_ssrc = self._audio_rtcp_ssrc = None
        self._video_rtcp_guest = self._audio_rtcp_guest = None
        self._dtmf: dict[int, tuple[int, int, bool, int]] = {}; self._last_dtmf_timestamp = None
        self._inbound_sent = False; self._directions = {"video": "sendrecv", "audio": "sendrecv"}
        self._h264_profile_level_id = _BASELINE_H264_PROFILE
        self._withheld_final_deadline: float | None = None
        self._peer_bye_cseq: int | None = None

    @staticmethod
    def _headers(message: bytes) -> tuple[list[str], bytes]:
        try:
            head, body = message.split(b"\r\n\r\n", 1); return head.decode("ascii", "strict").split("\r\n"), body
        except (ValueError, UnicodeDecodeError) as error: raise ProtocolRejected("malformed SIP message") from error

    @staticmethod
    def _header(lines: list[str], name: str) -> str:
        prefix = name.lower() + ":"
        for line in lines[1:]:
            if line.lower().startswith(prefix): return line.split(":", 1)[1].strip()
        raise ProtocolRejected(f"missing SIP header {name}")

    @staticmethod
    def _single_header(lines: list[str], name: str) -> str:
        prefix = name.lower() + ":"
        values = [line.split(":", 1)[1].strip() for line in lines[1:] if line.lower().startswith(prefix)]
        if len(values) != 1:
            raise ProtocolRejected(f"missing or duplicate SIP header {name}")
        return values[0]

    @staticmethod
    def _dialog_tag(value: str) -> str:
        if "<" in value or ">" in value:
            if value.count("<") != 1 or value.count(">") != 1 or value.index("<") > value.index(">"):
                raise ProtocolRejected("malformed SIP dialog identity")
            parameters = value[value.index(">") + 1:]
        else:
            parameters = value
        tags: list[str] = []
        for parameter in parameters.split(";")[1:]:
            key, separator, candidate = parameter.strip().partition("=")
            if key.lower() != "tag":
                continue
            if (not separator or not candidate or any(character not in "!%*+-.0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz~" for character in candidate)):
                raise ProtocolRejected("malformed SIP dialog tag")
            tags.append(candidate)
        if len(tags) != 1:
            raise ProtocolRejected("missing or duplicate SIP dialog tag")
        return tags[0]

    def _request(self, message: bytes) -> tuple[str, str, str, str, str, int, bytes]:
        lines, body = self._headers(message); parts = lines[0].split(" ") if lines else []
        if len(parts) != 3 or parts[2] != "SIP/2.0" or not parts[1].startswith("sip:") or not parts[0].isupper(): raise ProtocolRejected("malformed SIP request line")
        method = parts[0]; cseq = self._single_header(lines, "CSeq").split(" ")
        if len(cseq) != 2 or not cseq[0].isdigit() or cseq[1] != method: raise ProtocolRejected("invalid SIP CSeq")
        length = self._single_header(lines, "Content-Length")
        if not length.isdigit() or int(length) != len(body): raise ProtocolRejected("invalid SIP Content-Length")
        return method, self._single_header(lines, "Via"), self._single_header(lines, "From"), self._single_header(lines, "To"), self._single_header(lines, "Call-ID"), int(cseq[0]), body

    def _response(self, request: tuple[str, str, str, str, str, int, bytes], code: int, reason: str, body: str = "") -> bytes:
        method, via, from_value, to_value, call_id, cseq, _ = request
        if ";tag=" not in to_value: to_value += f";tag={_TAG}"
        content = body.encode("ascii"); headers = [f"SIP/2.0 {code} {reason}", f"Via: {via}", f"From: {from_value}", f"To: {to_value}", f"Call-ID: {call_id}", f"CSeq: {cseq} {method}", f"Contact: <sip:peer@{self.advertised_address}:{self.port};transport=udp>"]
        if body: headers.append("Content-Type: application/sdp")
        return ("\r\n".join(headers + [f"Content-Length: {len(content)}"]) + "\r\n\r\n").encode("ascii") + content

    def _directions_for(self, body: bytes) -> dict[str, str]:
        if not body: return {"video": "sendrecv", "audio": "sendrecv"}
        try: lines = body.decode("ascii", "strict").split("\r\n")
        except UnicodeDecodeError as error: raise ProtocolRejected("non-ASCII SDP") from error
        session, media, seen = "sendrecv", None, {}
        for line in lines:
            if line.startswith("m="): media = line[2:].split(" ", 1)[0]
            elif line in ("a=sendrecv", "a=sendonly", "a=recvonly", "a=inactive"):
                if media in ("video", "audio"): seen[media] = line[2:]
                elif media is None: session = line[2:]
        opposite = {"sendrecv":"sendrecv", "sendonly":"recvonly", "recvonly":"sendonly", "inactive":"inactive"}
        return {kind: opposite[seen.get(kind, session)] for kind in ("video", "audio")}

    @staticmethod
    def _h264_media_kind(line: str) -> str | None:
        fields = line[2:].split()
        media = fields[0] if fields else None
        if media == "video" and (len(fields) < 4 or "96" not in fields[3:]):
            raise ProtocolRejected("video offer omits H264 payload 96")
        return media

    @staticmethod
    def _h264_format(line: str) -> str:
        parameters = line[len("a=fmtp:96 "):].split(";")
        values: dict[str, str] = {}
        for parameter in parameters:
            key, separator, value = parameter.partition("=")
            if not separator or not key or not value or key in values:
                raise ProtocolRejected("invalid H264 payload 96 format")
            values[key] = value
        candidate = values.get("profile-level-id")
        if (values.get("packetization-mode") != "1" or candidate is None or
                len(candidate) != 6 or
                any(character not in "0123456789abcdefABCDEF" for character in candidate)):
            raise ProtocolRejected("invalid H264 profile-level-id")
        return candidate

    @staticmethod
    def _h264_profile_for(body: bytes) -> str:
        if not body:
            return _BASELINE_H264_PROFILE
        if len(body) > 8192:
            raise ProtocolRejected("SDP body exceeds peer limit")
        try:
            lines = body.decode("ascii", "strict").split("\r\n")
        except UnicodeDecodeError as error:
            raise ProtocolRejected("non-ASCII SDP") from error
        video_sections = 0
        video_payload_96 = False
        h264_payload_96 = False
        profile: str | None = None
        media: str | None = None
        for line in lines:
            if line.startswith("m="):
                media = SipRtpPeer._h264_media_kind(line)
                if media == "video":
                    video_sections += 1
                    video_payload_96 = True
                continue
            if media != "video":
                continue
            if line.startswith("a=rtpmap:96 "):
                if line != "a=rtpmap:96 H264/90000" or h264_payload_96:
                    raise ProtocolRejected("invalid H264 payload 96 mapping")
                h264_payload_96 = True
                continue
            if line.startswith("a=fmtp:96 "):
                if profile is not None:
                    raise ProtocolRejected("duplicate H264 payload 96 format")
                profile = SipRtpPeer._h264_format(line)
        return SipRtpPeer._validate_video_offer(video_sections, video_payload_96, h264_payload_96, profile)

    @staticmethod
    def _validate_video_offer(video_sections, video_payload_96, h264_payload_96, profile):
        if video_sections == 0:
            return _BASELINE_H264_PROFILE
        if video_sections != 1 or not video_payload_96 or not h264_payload_96 or profile is None:
            raise ProtocolRejected("video offer omits valid H264 payload 96")
        return profile

    def _sdp(self) -> str:
        sps = base64.b64encode(b"\x67" + bytes.fromhex(self._h264_profile_level_id)).decode("ascii")
        return (f"v=0\r\no=- 2 2 IN IP4 {self.advertised_address}\r\ns=ls200-private-peer\r\nc=IN IP4 {self.advertised_address}\r\nt=0 0\r\n"
                f"m=video {self.video.getsockname()[1]} RTP/AVP 96\r\na=rtcp-mux\r\na=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1;profile-level-id={self._h264_profile_level_id};sprop-parameter-sets={sps},aM4G4g==\r\na={self._directions['video']}\r\n"
                f"m=audio {self.audio.getsockname()[1]} RTP/AVP 0 8 101\r\na=rtcp-mux\r\na=rtpmap:0 PCMU/8000\r\na=rtpmap:8 PCMA/8000\r\na=rtpmap:101 telephone-event/8000\r\na=fmtp:101 0-16\r\na={self._directions['audio']}\r\n")

    def _accept_invite(self, request, address) -> None:
        _, _, from_value, to_value, call_id, cseq, body = request
        if self._sip_state != "idle": raise ProtocolRejected("second SIP dialog")
        profile_level_id = self._h264_profile_for(body)
        self._sip_guest, self._call_id, self._from, self._to, self._invite_cseq = address, call_id, from_value, to_value + f";tag={_TAG}", cseq
        self._invite_request = request
        self._h264_profile_level_id = profile_level_id
        self._directions = ({"video": "inactive", "audio": "inactive"}
                            if self.signaling_only else self._directions_for(body)); self._sip_state = "invited"; self.evidence.invite_seen = True; self.evidence.dialogs_started += 1; self.evidence.sip_source = address; self.evidence.invite_at_seconds = time.monotonic() - self._started
        withhold_final = self.withhold_first_invite_final and not self._first_invite_final_withheld
        if withhold_final:
            self._first_invite_final_withheld = True
            self._withheld_final_deadline = time.monotonic() + self.withheld_final_timeout
            self.evidence.withheld_final_invites += 1
            self.evidence.withheld_final_at_seconds = time.monotonic() - self._started
        for code, reason in ((100, "Trying"), (180, "Ringing")):
            self.sip.sendto(self._response(request, code, reason), address)
        if not withhold_final:
            self.sip.sendto(self._response(request, 200, "OK", self._sdp()), address)

    def _correlated_dialog(self, address, call_id, from_value, to_value):
        return (address == self._sip_guest and call_id == self._call_id
                and from_value == self._from and to_value == self._to)

    def _handle_dialog_request(self, request, address) -> None:
        method, _, from_value, to_value, call_id, cseq, _ = request
        if method == "CANCEL":
            self._handle_cancel(request, address)
            return
        if not self._correlated_dialog(address, call_id, from_value, to_value): raise ProtocolRejected("uncorrelated SIP dialog")
        if method == "ACK" and self._sip_state == "invited" and cseq == self._invite_cseq:
            self._sip_state = "acked"; self.evidence.ack_seen = True; self.evidence.ack_at_seconds = time.monotonic() - self._started
        elif method == "BYE" and self._sip_state == "acked" and cseq > self._invite_cseq:
            self.sip.sendto(self._response(request, 200, "OK"), address); self.evidence.bye_seen = True; self.evidence.dialogs_completed += 1; self.evidence.bye_at_seconds = time.monotonic() - self._started; self._clear_dialog()
        elif method == "OPTIONS": self.sip.sendto(self._response(request, 200, "OK"), address)
        else: raise ProtocolRejected("unsupported or out-of-order SIP method")

    def _handle_cancel(self, request, address) -> None:
        # RFC 3261 section 9: CANCEL copies the INVITE transaction headers,
        # including the original To value, rather than the response's To tag.
        invite = self._invite_request
        if invite is None or address != self._sip_guest or request[1:6] != invite[1:6]:
            raise ProtocolRejected("uncorrelated SIP CANCEL")
        self.sip.sendto(self._response(request, 200, "OK"), address)
        if self._withheld_final_deadline is not None:
            self.sip.sendto(self._response(invite, 487, "Request Terminated"), address)
            self.evidence.withheld_final_cancels += 1
            self._clear_dialog()

    def _handle_sip(self) -> None:
        message, address = self.sip.recvfrom(65535)
        if address[0] != self.authorized_source_address: self.evidence.rejected_sip_packets += 1; return
        try:
            if message.startswith(b"SIP/2.0 "):
                self._handle_peer_bye_response(message, address)
                return
            request = self._request(message); method, _, from_value, to_value, call_id, cseq, body = request
            if method == "INVITE":
                self._accept_invite(request, address)
                return
            self._handle_dialog_request(request, address)
        except ProtocolRejected: self.evidence.rejected_sip_packets += 1

    def _handle_peer_bye_response(self, message: bytes, address: tuple[str, int]) -> None:
        lines, body = self._headers(message)
        parts = lines[0].split(" ") if lines else []
        if len(parts) != 3 or parts[0] != "SIP/2.0" or parts[1] != "200":
            raise ProtocolRejected("unexpected SIP response")
        length = self._header(lines, "Content-Length")
        if not length.isdigit() or int(length) != len(body):
            raise ProtocolRejected("invalid SIP response Content-Length")
        cseq = self._single_header(lines, "CSeq").split(" ")
        call_id = self._single_header(lines, "Call-ID")
        from_value = self._single_header(lines, "From")
        to_value = self._single_header(lines, "To")
        if (self._sip_state != "peer_bye_sent" or address != self._sip_guest or
                self._peer_bye_cseq is None or cseq != [str(self._peer_bye_cseq), "BYE"] or
                call_id != self._call_id or
                self._dialog_tag(from_value) != self._dialog_tag(self._to) or
                self._dialog_tag(to_value) != self._dialog_tag(self._from)):
            raise ProtocolRejected("uncorrelated peer BYE response")
        self.evidence.peer_bye_200_seen += 1
        self.evidence.peer_bye_200_at_seconds = time.monotonic() - self._started
        self.evidence.dialogs_completed += 1
        self._clear_dialog()

    @staticmethod
    def _valid_rtcp(packet: bytes) -> bool:
        offset = 0
        while offset < len(packet):
            if offset + 4 > len(packet) or packet[offset] >> 6 != 2 or not 192 <= packet[offset + 1] <= 223: return False
            size = (struct.unpack_from("!H", packet, offset + 2)[0] + 1) * 4
            minimum = {200: 28, 201: 8}.get(packet[offset + 1], 8)
            if packet[offset + 1] in (200, 201):
                minimum += (packet[offset] & 0x1f) * 24
            if size < minimum or offset + size > len(packet): return False
            offset += size
        return offset == len(packet) and len(packet) >= 4

    @staticmethod
    def _rtp_fields(packet: bytes) -> tuple[int, int, int, bytes]:
        if len(packet) < 12 or packet[0] >> 6 != 2:
            raise ProtocolRejected("malformed RTP")
        header = 12 + (packet[0] & 0x0f) * 4
        if len(packet) < header:
            raise ProtocolRejected("truncated RTP CSRC list")
        if packet[0] & 0x10:
            if len(packet) < header + 4:
                raise ProtocolRejected("truncated RTP extension")
            header += 4 + struct.unpack_from("!H", packet, header + 2)[0] * 4
            if len(packet) < header:
                raise ProtocolRejected("truncated RTP extension data")
        end = len(packet)
        if packet[0] & 0x20:
            padding = packet[-1]
            if padding == 0 or padding > end - header:
                raise ProtocolRejected("invalid RTP padding")
            end -= padding
        if end == header:
            raise ProtocolRejected("empty RTP payload")
        return packet[1] & 0x7f, struct.unpack_from("!I", packet, 4)[0], struct.unpack_from("!I", packet, 8)[0], packet[header:end]

    def _receive_rtcp(self, packet, address, video, expected, ssrc) -> None:
        expected_rtcp = (
            self._video_rtcp_guest if video else self._audio_rtcp_guest
        ) if self.allow_distinct_rtcp_tuple else expected
        if expected_rtcp is not None and address != expected_rtcp:
            raise ProtocolRejected("RTCP tuple changed")
        if not self._valid_rtcp(packet): raise ProtocolRejected("malformed RTCP")
        sender = struct.unpack_from("!I", packet, 4)[0]
        if ssrc is not None and sender != ssrc: raise ProtocolRejected("RTCP SSRC changed")
        if video:
            if self.allow_distinct_rtcp_tuple: self._video_rtcp_guest = address
            else: self._video_guest = address
            self._video_rtcp_ssrc = sender; self.evidence.outbound_video_rtcp_packets += 1
        else:
            if self.allow_distinct_rtcp_tuple: self._audio_rtcp_guest = address
            else: self._audio_guest = address
            self._audio_rtcp_ssrc = sender; self.evidence.outbound_audio_rtcp_packets += 1

    def _receive_payload(self, packet, address, video, payload_type, timestamp, sender, payload) -> None:
        if video:
            if payload_type != 96 or len(packet) == 12: raise ProtocolRejected("unexpected H264 RTP")
            if self._video_rtcp_ssrc is not None and sender != self._video_rtcp_ssrc: raise ProtocolRejected("RTP/RTCP SSRC changed")
            self._video_guest = address; self._video_ssrc = sender; self.evidence.video_source = address; self.evidence.video_ssrc = sender; self.evidence.outbound_video_packets += 1; self._video_hash.update(packet)
        else:
            if payload_type not in (0, 8, 101) or len(packet) == 12: raise ProtocolRejected("unexpected audio RTP")
            if self._audio_rtcp_ssrc is not None and sender != self._audio_rtcp_ssrc: raise ProtocolRejected("RTP/RTCP SSRC changed")
            self._audio_guest = address; self._audio_ssrc = sender; self.evidence.audio_source = address; self.evidence.audio_ssrc = sender; self.evidence.outbound_audio_packets += 1; self._audio_hash.update(packet)
            if payload_type == 101:
                self._receive_dtmf(timestamp, payload)

    def _receive_dtmf(self, timestamp, payload) -> None:
        if len(payload) != 4 or payload[0] > 16 or payload[1] & 0x40: raise ProtocolRejected("malformed RFC4733")
        event, duration, ended = payload[0], struct.unpack_from("!H", payload, 2)[0], bool(payload[1] & 0x80); prior = self._dtmf.get(timestamp)
        if not duration or prior and (prior[0] != event or duration < prior[1] or prior[2] and not ended): raise ProtocolRejected("inconsistent RFC4733")
        repeats = (prior[3] if prior else 0) + int(ended); self._dtmf[timestamp] = (event, duration, ended, repeats); self.evidence.dtmf_tone = event
        if repeats >= 3 and timestamp != self._last_dtmf_timestamp: self.evidence.dtmf_tones.append(event); self._last_dtmf_timestamp = timestamp

    def _handle_rtp(self, descriptor: socket.socket, video: bool) -> None:
        packet, address = descriptor.recvfrom(65535)
        if self._sip_state != "acked" or address[0] != self.authorized_source_address or not self.ls200_media_port_min <= address[1] <= self.ls200_media_port_max: self.evidence.rejected_media_packets += 1; return
        try:
            expected, ssrc = (self._video_guest, self._video_ssrc) if video else (self._audio_guest, self._audio_ssrc)
            if len(packet) >= 2 and 192 <= packet[1] <= 223:
                self._receive_rtcp(packet, address, video, expected, ssrc)
                return
            if expected is not None and address != expected: raise ProtocolRejected("media tuple changed")
            payload_type, timestamp, sender, payload = self._rtp_fields(packet)
            if sender != ssrc and ssrc is not None: raise ProtocolRejected("RTP SSRC changed")
            self._receive_payload(packet, address, video, payload_type, timestamp, sender, payload)
        except ProtocolRejected: self.evidence.rejected_media_packets += 1

    def _send_inbound_media(self) -> None:
        if self._inbound_sent or self._video_guest is None or self._audio_guest is None: return
        sps = b"\x67" + bytes.fromhex(self._h264_profile_level_id)
        for sequence, nal in enumerate((sps, b"\x68\xce\x06\xe2", b"\x65\x88\x84\x21"), 1): self.video.sendto(_rtp(96, sequence, 9000, 0x515A2001, nal, sequence == 3), self._video_guest); self.evidence.inbound_video_packets += 1
        for sequence in range(1, 6): self.audio.sendto(_rtp(0, sequence, sequence * 160, 0x515A2002, b"\xff" * 160), self._audio_guest); self.evidence.inbound_audio_packets += 1
        self._inbound_sent = True

    def _media_proven(self) -> bool:
        evidence = self.evidence
        return (
            evidence.outbound_video_packets > 0 and evidence.outbound_audio_packets > 0
            and evidence.outbound_video_rtcp_packets > 0 and evidence.outbound_audio_rtcp_packets > 0
            and evidence.inbound_video_packets > 0 and evidence.inbound_audio_packets > 0
        )

    def _maybe_send_peer_bye(self) -> None:
        if not (self.peer_initiated_hangup and self._sip_state == "acked" and self._media_proven()):
            return
        if self._sip_guest is None or self._call_id is None or self._from is None or self._to is None or self._invite_cseq is None:
            raise RuntimeError("established SIP dialog is incomplete")
        self._peer_bye_cseq = self._invite_cseq + 1
        message = (
            f"BYE sip:peer@{self.authorized_source_address} SIP/2.0\r\n"
            f"Via: SIP/2.0/UDP {self.advertised_address}:{self.port};branch=z9hG4bK-ls200-peer\r\n"
            f"From: {self._to}\r\nTo: {self._from}\r\nCall-ID: {self._call_id}\r\n"
            f"CSeq: {self._peer_bye_cseq} BYE\r\nContent-Length: 0\r\n\r\n"
        ).encode("ascii")
        self.sip.sendto(message, self._sip_guest)
        self._sip_state = "peer_bye_sent"
        self.evidence.peer_bye_sent += 1
        self.evidence.peer_bye_at_seconds = time.monotonic() - self._started

    def _expire_withheld_invite(self) -> None:
        if self._withheld_final_deadline is not None and time.monotonic() >= self._withheld_final_deadline:
            self.evidence.withheld_final_timeouts += 1
            self._clear_dialog()

    def _run(self) -> None:
        try:
            while not self._stop.is_set():
                ready, _, _ = select.select((self.sip, self.video, self.audio), (), (), .05)
                for descriptor in ready: self._handle_sip() if descriptor is self.sip else self._handle_rtp(descriptor, descriptor is self.video)
                self._expire_withheld_invite()
                self._send_inbound_media()
                self._maybe_send_peer_bye()
        except BaseException as error:
            if not self._stop.is_set(): self._error = error
            self._stop.set()

    def wait_for(self, predicate: Callable[[PeerEvidence], bool], timeout: float, label: str) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.assert_healthy()
            if predicate(self.evidence): return
            time.sleep(.05)
        raise TimeoutError(f"timed out waiting for {label}: {self.evidence}")
