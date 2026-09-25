import { Grid2X2, KeyRound, Keyboard, MicOff, PhoneCall, PhoneOff, RefreshCw, Video, VideoOff } from 'lucide-react'
import { useCallback, useState } from 'react'
import { api } from '../api'
import { CallEvidence, SourceCounters } from '../components/CallEvidence'
import { previewLabel, projectActiveCall, projectMedia } from '../statusProjection'
import type { ApiError, CallRequest, JsonRecord, Role } from '../types'
import { isRecord, readNumber, readText } from '../types'
import { Button, EmptyState, ErrorNotice, Freshness, LoadingRows, Notice, ValueRow } from '../components/ui'
import { useLivePolling } from '../hooks/useLivePolling'

type CallData = { active: JsonRecord | null; calls: JsonRecord | null; directory: JsonRecord | null; media: JsonRecord | null; status: JsonRecord | null; preview: JsonRecord | null }
const EMPTY_FORM: CallRequest = { meeting_id: '', profile: 'zoom_direct', passcode: '', layout: 'full_screen', host_key: '', dial_code: '' }
const KEYS = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '*', '0', '#']

export function CallsScreen({ role, onConnection, embedded = false }: { embedded?: boolean; role: Role; onConnection: (state: 'live' | 'stale' | 'offline') => void }) {
  const [form, setForm] = useState<CallRequest>(EMPTY_FORM)
  const [actionError, setActionError] = useState<ApiError | null>(null)
  const [message, setMessage] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)
  const [keypadOpen, setKeypadOpen] = useState(false)

  const loadCalls = useCallback(async (signal: AbortSignal): Promise<CallData> => {
    const [status, calls, directory, preview] = await Promise.all([api.status(signal), api.calls(signal), api.directory(signal), api.preview(signal).catch(() => null)])
    return { active: projectActiveCall(status.data), calls: calls.data, directory: directory.data, media: projectMedia(status.data), status: status.data, preview: preview?.data ?? null }
  }, [])
  const { data, error, refresh } = useLivePolling({ load: loadCalls, onConnection })

  const active = data?.active
  const callState = readText(active, 'call_state') ?? readText(data?.status ?? null, 'call_state') ?? 'unknown'
  const activeCall = ['active', 'connected', 'connecting', 'resolving', 'inviting',
    'early', 'establishing_media', 'established', 'terminating'].includes(callState)
  const displayName = readText(active, 'display_name') ?? readText(active, 'meeting_name') ?? readText(active, 'meeting_id')
  const canOperate = role === 'operator' || role === 'admin'
  const videoEnabled = typeof data?.status?.video_transmit_enabled === 'boolean'
    ? data.status.video_transmit_enabled : null
  const audioMuted = typeof data?.status?.audio_muted === 'boolean'
    ? data.status.audio_muted : null
  const elapsed = readNumber(active, 'elapsed_seconds')
  const directoryEntries = isRecord(data?.directory) && Array.isArray(data.directory.entries)
    ? data.directory.entries.filter(isRecord) : []

  const updateForm = <K extends keyof CallRequest>(key: K, value: CallRequest[K]) => setForm(previous => ({ ...previous, [key]: value }))
  const perform = async (operation: () => Promise<unknown>, success: string, onSuccess?: () => void) => {
    setBusy(true); setActionError(null); setMessage(null)
    try { await operation(); onSuccess?.(); setMessage(success); await refresh() } catch (cause) { setActionError(cause as ApiError) } finally { setBusy(false) }
  }
  const onJoin = (event: React.FormEvent) => {
    event.preventDefault()
    void perform(() => api.joinCall(form), 'Join request sent to the device.', () => {
      setForm(previous => ({ ...previous, passcode: '', host_key: '', dial_code: '' }))
    })
  }
  const onTone = (tone: string) => void perform(() => api.sendDtmf(tone), `Tone ${tone} sent.`)
  const onVideo = () => {
    if (videoEnabled === null) return
    const next = videoEnabled ? 'disabled' : 'enabled'
    void perform(() => api.setVideo(next), `Video transmit ${next}.`)
  }
  const onAudioMute = () => {
    if (audioMuted === null) return
    const next = !audioMuted
    void perform(() => api.setAudioMute(next), next ? 'Outbound audio muted.' : 'Outbound audio unmuted.')
  }

  return <section className={embedded ? "room-zoom" : undefined} aria-labelledby="calls-title"><div className="page-heading"><div>{embedded ? <h2 id="calls-title">Zoom</h2> : <><h1 id="calls-title">Calls</h1><p>Live call state is supplied by the device.</p></>}</div><Button onClick={() => void refresh()}><RefreshCw aria-hidden="true" size={17} /> Refresh</Button></div><ErrorNotice error={actionError ?? error} />{message && <Notice tone="success">{message}</Notice>}
    {!data ? <LoadingRows count={7} /> : <div className="calls-grid">
      <details className="join-panel" open={embedded ? undefined : true}><summary id="join-title">Join a meeting</summary><form className="form-grid" onSubmit={onJoin}>
        <label>Directory <select value="" onChange={event => {
          const entry = directoryEntries.find(item => readText(item, 'id') === event.target.value)
          if (!entry) return
          setForm(previous => ({ ...previous,
            meeting_id: readText(entry, 'meeting_id') ?? previous.meeting_id,
            profile: (readText(entry, 'profile') ?? previous.profile) as CallRequest['profile'],
            layout: (readText(entry, 'default_layout') ?? previous.layout) as CallRequest['layout']
          }))
        }} disabled={!canOperate || busy || directoryEntries.length === 0}><option value="">{directoryEntries.length === 0 ? 'No saved meetings' : 'Choose a saved meeting'}</option>{directoryEntries.map(entry => <option key={readText(entry, 'id') ?? readText(entry, 'meeting_id') ?? 'entry'} value={readText(entry, 'id') ?? ''}>{readText(entry, 'name') ?? 'Saved meeting'}</option>)}</select></label>
        <label>Meeting ID<input inputMode="numeric" pattern="[0-9]{9,11}" value={form.meeting_id} onChange={event => updateForm('meeting_id', event.target.value.replace(/\D/g, ''))} placeholder="9 to 11 digits" minLength={9} maxLength={11} required disabled={!canOperate || busy} /><small>Numbers only. This device accepts 9 to 11 digits.</small></label>
        <label>Passcode <span className="optional">optional</span><input type="password" inputMode="numeric" pattern="[0-9]*" value={form.passcode} onChange={event => updateForm('passcode', event.target.value.replace(/\D/g, ''))} maxLength={64} disabled={!canOperate || busy} /></label>
        <label>{embedded ? 'Meeting layout' : 'Layout'}<select value={form.layout} onChange={event => updateForm('layout', event.target.value as CallRequest['layout'])} disabled={!canOperate || busy}><option value="full_screen">Full screen</option><option value="gallery">Gallery</option><option value="dual_video">Dual video</option></select></label>
        <label>Host key <span className="optional">optional</span><input type="password" inputMode="numeric" value={form.host_key} onChange={event => updateForm('host_key', event.target.value.replace(/\D/g, ''))} maxLength={10} disabled={!canOperate || busy} /></label>
        <label>Dial or participant code <span className="optional">optional</span><input type="password" pattern="[A-Za-z0-9]+" value={form.dial_code} onChange={event => updateForm('dial_code', event.target.value.replace(/[^A-Za-z0-9]/g, ''))} maxLength={64} disabled={!canOperate || busy} /><small>Opaque Zoom-issued alphanumeric value. Delimiters are not accepted.</small></label>
        <label>Profile<select value={form.profile} onChange={event => updateForm('profile', event.target.value as CallRequest['profile'])} disabled={!canOperate || busy}><option value="zoom_direct">Zoom direct</option><option value="zoom_proxy">Zoom proxy</option><option value="private_lab">Private lab</option></select></label>
        {!canOperate && <Notice tone="info">Your {role} role can view call state but cannot start a call.</Notice>}
        <Button variant="primary" type="submit" disabled={!canOperate || busy}><PhoneCall aria-hidden="true" size={18} /> Join call</Button>
      </form>
      <h2 className="section-gap">Safe recents</h2><SafeRecents calls={data.calls} />
      </details>
      <section className="active-panel" aria-labelledby="active-title"><h2 id="active-title">Active call</h2>{activeCall ? <>
        <div className="active-call"><span className="active-call__icon"><Video aria-hidden="true" size={28} /></span><div><h3>{displayName ?? (['connected', 'active', 'established'].includes(callState) ? 'Connected call' : 'Call in progress')}</h3><Freshness state={!error && ['connected', 'active', 'established'].includes(callState) ? 'live' : 'stale'} label={`${error ? 'Last reported: ' : ''}${callState}${elapsed !== null ? ` · ${formatDuration(elapsed)}` : ''}`} /><div className="protocol-list"><span><KeyRound aria-hidden="true" size={17} />{readText(isRecord(active?.sip) ? active.sip : null, 'transport') ?? 'Transport not reported'}</span><span><Video aria-hidden="true" size={17} />{readText(data.media, 'video_codec') ?? 'Video codec not reported'}</span><span><MicOff aria-hidden="true" size={17} />{readText(data.media, 'audio_codec') ?? 'Audio codec not reported'}</span></div></div></div>
        <div className="control-grid"><Button onClick={() => setKeypadOpen(value => !value)} disabled={!canOperate}><Keyboard aria-hidden="true" size={20} /> Keypad</Button><Button variant={audioMuted ? 'primary' : 'danger'} onClick={onAudioMute} disabled={!canOperate || busy || audioMuted === null} title={audioMuted === null ? 'The device did not report audio mute state.' : undefined}><MicOff aria-hidden="true" size={20} />{audioMuted === null ? 'Mute state unavailable' : audioMuted ? (embedded ? 'Unmute meeting' : 'Unmute audio') : (embedded ? 'Mute meeting' : 'Mute audio')}</Button><Button onClick={onVideo} disabled={!canOperate || busy || videoEnabled === null} title={videoEnabled === null ? 'The device did not report video transmit state.' : undefined}>{videoEnabled === false ? <Video aria-hidden="true" size={20} /> : <VideoOff aria-hidden="true" size={20} />}{videoEnabled === null ? 'Video state unavailable' : videoEnabled ? 'Stop video' : 'Start video'}</Button><Button onClick={() => void perform(api.cycleLayout, 'Zoom layout cycle requested.')} disabled={!canOperate || busy}><Grid2X2 aria-hidden="true" size={20} /> {embedded ? 'Next meeting layout' : 'Next layout'}</Button><Button className="span-two" onClick={() => void perform(api.requestKeyframe, 'Local keyframe requested.')} disabled={!canOperate || busy}><RefreshCw aria-hidden="true" size={20} /> Request keyframe</Button></div>
        {keypadOpen && <div className="keypad" aria-label="DTMF keypad">{KEYS.map(tone => <Button key={tone} onClick={() => onTone(tone)} disabled={!canOperate || busy}>{tone}</Button>)}</div>}
        <Button variant="danger" className="hangup" onClick={() => void perform(api.hangup, 'Hang-up request sent.')} disabled={!canOperate || busy}><PhoneOff aria-hidden="true" size={20} /> Hang up</Button>
      </> : <EmptyState title={callState === 'idle' ? 'No active call' : 'Call state unavailable'}>{callState === 'idle' ? 'Use the form to start a permitted meeting.' : 'The device did not report an active-call record.'}</EmptyState>}</section>
      {!embedded && <LiveHealth media={data.media} status={data.status} preview={data.preview} stale={error !== null} />}
    </div>}</section>
}

function SafeRecents({ calls }: { calls: JsonRecord | null }) {
  const values = isRecord(calls) && Array.isArray(calls.recents) ? calls.recents.filter(isRecord) : []
  if (values.length === 0) return <EmptyState title="No safe recent calls">The device has not reported non-sensitive recent-call metadata.</EmptyState>
  return <ul className="open-list">{values.map((item, index) => <li key={`${readText(item, 'id') ?? 'recent'}-${index}`}><span>{readText(item, 'name') ?? 'Recent call'}</span><small>{[readText(item, 'profile'), readText(item, 'default_layout')].filter(Boolean).join(' · ') || 'Call policy not reported'}</small></li>)}</ul>
}

function LiveHealth({ media, status, preview, stale }: { media: JsonRecord | null; status: JsonRecord | null; preview: JsonRecord | null; stale: boolean }) {
  return <aside className="health-rail" aria-labelledby="health-title"><h2 id="health-title">Live health</h2>
    <CallEvidence status={status} stale={stale} />
    <ValueRow label="Snapshot" value={stale ? 'Stale — refresh failed' : 'Latest authenticated response'} />
    <ValueRow label="Media security" value={readText(media, 'rtp') ?? 'No negotiated security reported'} />
    <ValueRow label="Audio direction" value={readText(media, 'audio_direction')} />
    <ValueRow label="Video direction" value={readText(media, 'video_direction')} />
    <SourceCounters status={status} />
    <div className="preview-placeholder"><Video aria-hidden="true" size={32} /><strong>Local preview</strong><span>{previewLabel(preview)}</span></div>
    <p className="honesty-note">Local preview is independent of the SIP call. Receive rendering describes device output, not outgoing media.</p>
    <ValueRow label="Receive audio renderer" value={readText(media, 'audio_renderer') ?? 'Not reported'} />
    <ValueRow label="Receive video renderer" value={readText(media, 'video_renderer') ?? 'Not reported'} />
    <ValueRow label="AEC" value={readText(media, 'aec') ?? 'Not reported'} />
    <p className="honesty-note">Round-trip timing and remote reception are not reported by this status API.</p>
  </aside>
}

function formatDuration(seconds: number) { const minutes = Math.floor(seconds / 60); return `${minutes}:${String(seconds % 60).padStart(2, '0')}` }
