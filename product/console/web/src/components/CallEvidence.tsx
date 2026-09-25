import { connectionLabel } from '../statusProjection'
import { isRecord, readNumber, readText, type JsonRecord } from '../types'
import { ValueRow } from './ui'

export function CallEvidence({ status, stale = false }: { status: JsonRecord | null; stale?: boolean }) {
  const sip = isRecord(status?.sip) ? status.sip : null
  return <><ValueRow label="Call connection" value={stale ? 'Unknown — last snapshot stale' : connectionLabel(status)} /><ValueRow label="SIP transport" value={readText(sip, 'transport') ?? 'Not reported'} /><ValueRow label="Media session" value={readText(status, 'media_state') ?? 'Not reported'} /><p className="honesty-note">An established SIP call confirms signaling. It does not confirm that the remote participant receives audio or video.</p></>
}

export function SourceCounters({ status }: { status: JsonRecord | null }) {
  const audio = isRecord(status?.audio) ? status.audio : null
  const video = isRecord(status?.video) ? status.video : null
  const frames = readNumber(audio, 'frames') ?? readNumber(video, 'frames')
  return <><ValueRow label="Source frames read" value={frames?.toString() ?? 'Not reported'} /><p className="honesty-note">Audio and video share this source counter. It measures local reads; remote reception is not reported.</p></>
}
