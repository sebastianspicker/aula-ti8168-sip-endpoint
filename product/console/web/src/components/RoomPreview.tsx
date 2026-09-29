import { useCallback, useRef } from 'react'
import { Video } from 'lucide-react'
import { api } from '../api'
import { PREVIEW_STREAM_PATH, usePreviewPlayer } from '../hooks/usePreviewPlayer'
import { useLivePolling } from '../hooks/useLivePolling'
import { readText } from '../types'

const ignoreConnection = () => undefined

export function RoomPreview() {
  const load = useCallback(async (signal: AbortSignal) => (await api.preview(signal)).data, [])
  const { data, error } = useLivePolling({ load, onConnection: ignoreConnection })
  const video = useRef<HTMLVideoElement>(null)
  const path = readText(data, 'stream_path')
  const state = readText(data, 'state') ?? 'unknown'
  const usable = !error && path === PREVIEW_STREAM_PATH && ['idle', 'live'].includes(state)
  const player = usePreviewPlayer(video, usable ? path : null, state)
  return <section className="preview-frame room-preview" aria-labelledby="room-preview-title">
    <div className="preview-frame__head"><h2 id="room-preview-title">Local preview</h2><span>{usable ? player : data || error ? 'Unavailable' : 'Connecting'}</span></div>
    {usable ? <video ref={video} aria-label="Authenticated local Aula source" muted playsInline controls /> : <div className="preview-empty"><Video aria-hidden="true" size={38} /><h3>{data || error ? 'Local preview unavailable' : 'Connecting to local preview'}</h3><p>{error ? 'The authenticated preview could not be refreshed.' : 'The device has not supplied an authenticated preview stream.'}</p></div>}
  </section>
}
