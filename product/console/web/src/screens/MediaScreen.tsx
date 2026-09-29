import { AudioLines, Monitor, RefreshCw, Video } from 'lucide-react'
import { useCallback, useRef } from 'react'
import { api } from '../api'
import { CallEvidence, SourceCounters } from '../components/CallEvidence'
import { previewLabel, projectMedia } from '../statusProjection'
import type { JsonRecord } from '../types'
import { readText } from '../types'
import { Button, ErrorNotice, LoadingRows, Notice, ValueRow } from '../components/ui'
import { useLivePolling } from '../hooks/useLivePolling'
import { PREVIEW_STREAM_PATH, usePreviewPlayer } from '../hooks/usePreviewPlayer'

type MediaData = { status: JsonRecord; media: JsonRecord; preview: JsonRecord | null }

export function MediaScreen({ onConnection }: { onConnection: (state: 'live' | 'stale' | 'offline') => void }) {
  const loadMedia = useCallback(async (signal: AbortSignal): Promise<MediaData> => {
    const [status, preview] = await Promise.all([api.status(signal), api.preview(signal).catch(() => null)])
    return { status: status.data, media: projectMedia(status.data), preview: preview?.data ?? null }
  }, [])
  const { data, error, refresh } = useLivePolling({ load: loadMedia, onConnection })
  const media = data?.media ?? null
  const preview = data?.preview ?? null
  const video = useRef<HTMLVideoElement>(null)
  const reportedPath = readText(preview, 'stream_path')
  const streamPath = reportedPath === PREVIEW_STREAM_PATH ? reportedPath : null
  const previewState = readText(preview, 'state') ?? 'Not reported'
  const playerState = usePreviewPlayer(video, streamPath, previewState)
  return <section aria-labelledby="media-title"><div className="page-heading"><div><h1 id="media-title">Media</h1><p>Negotiated details are reported by the authenticated device API.</p></div><Button onClick={() => void refresh()}><RefreshCw aria-hidden="true" size={17} /> Refresh</Button></div><ErrorNotice error={error} />{!media ? <LoadingRows count={6} /> : <div className="media-layout"><div><section className="preview-frame" aria-labelledby="preview-title"><div className="preview-frame__head"><h2 id="preview-title">Authenticated local preview</h2><span>{playerState === 'idle' ? previewLabel(preview) : playerState}</span></div>{streamPath && ['idle', 'live'].includes(previewState) ? <video ref={video} aria-label="Authenticated local Aula source" muted playsInline controls /> : <div className="preview-empty"><Video aria-hidden="true" size={42} /><h3>Local preview unavailable</h3><p>{reportedPath && streamPath === null ? 'The device returned an invalid preview path.' : preview?.reason === 'not_configured' ? 'Local preview is not configured in the gateway. It is independent of the SIP call.' : 'The device did not provide an authenticated preview stream.'}</p></div>}</section><Notice tone="info"><Monitor aria-hidden="true" size={18} /> Receive rendering describes device output. Send-only calls do not require a receive renderer; local preview does not verify outgoing meeting media.</Notice></div><aside className="readiness" aria-labelledby="readiness-title"><h2 id="readiness-title">Media status</h2><CallEvidence status={data?.status ?? null} stale={error !== null} /><ValueRow label="Audio direction" value={readText(media, 'audio_direction')} /><ValueRow label="Video direction" value={readText(media, 'video_direction')} /><SourceCounters status={data?.status ?? null} /><ValueRow label="Video codec" value={readText(media, 'video_codec') ?? 'Not negotiated'} /><ValueRow label="Audio codec" value={readText(media, 'audio_codec') ?? 'Not negotiated'} /><ValueRow label="Media security" value={readText(media, 'rtp') ?? 'Not reported'} /><ValueRow label="Audio level" value={readText(media, 'audio_level') ?? 'Not reported'} /><ValueRow label="Receive audio renderer" value={readText(media, 'audio_renderer') ?? 'Not reported'} /><ValueRow label="Receive video renderer" value={readText(media, 'video_renderer') ?? 'Not reported'} /><ValueRow label="AEC" value={readText(media, 'aec') ?? 'Not reported'} /><ValueRow label="HDMI" value={readText(media, 'hdmi') ?? 'Not reported'} /><div className="audio-level"><AudioLines aria-hidden="true" size={20} /><span>Audio telemetry is never animated by the console.</span></div></aside></div>}</section>
}
