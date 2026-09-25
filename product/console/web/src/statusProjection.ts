import { isRecord, readNumber, readText, type JsonRecord } from './types'

const record = (value: unknown): JsonRecord | null => isRecord(value) ? value : null
export function projectActiveCall(status: JsonRecord): JsonRecord { return status }
export function connectionLabel(status: JsonRecord | null): string {
  const state = readText(status, 'call_state')
  if (['established', 'connected', 'active'].includes(state ?? '')) return 'SIP call established'
  if (state === 'idle') return 'Idle'
  return state ? `Call ${state.replaceAll('_', ' ')}` : 'Call state not reported'
}

function renderingState(value: unknown, stream: JsonRecord | null): string | undefined {
  if (stream?.direction === 'sendonly') return 'Not expected (send only)'
  if (stream?.direction === 'inactive') return 'Inactive stream'
  const renderer = record(value)
  if (!renderer) return undefined
  return renderer.rendering === true && renderer.fresh === true ? 'healthy' : 'not rendering'
}

function securityState(audio: JsonRecord | null, video: JsonRecord | null) {
  const values = [audio, video].filter(stream => stream && stream.direction !== 'inactive').map(stream => readText(stream, 'security'))
  if (!values.length || values.some(value => !['rtp', 'srtp'].includes(value ?? ''))) return undefined
  return values.every(value => value === 'srtp') ? 'srtp' : values.every(value => value === 'rtp') ? 'rtp' : 'mixed srtp/rtp'
}

export function directionLabel(value: unknown): string {
  return ({ sendonly: 'Send only', recvonly: 'Receive only', sendrecv: 'Send and receive', inactive: 'Inactive' } as Record<string, string>)[String(value)] ?? 'Not reported'
}

function videoCodec(video: JsonRecord | null): string | undefined {
  if (!video || !['sendonly', 'recvonly', 'sendrecv'].includes(String(video.direction))) return undefined
  return /^[0-9a-f]{6}$/i.test(readText(video, 'h264_profile_level_id') ?? '') ? 'H.264' : undefined
}

function audioCodec(audio: JsonRecord | null): string | undefined {
  if (!audio || audio.direction === 'inactive') return undefined
  const codecs: Record<number, string> = { 0: 'G.711 μ-law', 8: 'G.711 A-law' }
  return codecs[readNumber(audio, 'payload_type') ?? -1]
}

function rendererHealth(renderer: JsonRecord | null): string | undefined {
  if (!renderer) return undefined
  return renderer.healthy === true ? 'healthy' : 'unavailable'
}

export function projectMedia(status: JsonRecord): JsonRecord {
  const renderer = record(status.renderer)
  const audio = record(status.audio)
  const video = record(status.video)
  const established = ['established', 'connected', 'active'].includes(readText(status, 'call_state') ?? '')
  return {
    video_codec: established ? videoCodec(video) : undefined,
    audio_codec: established ? audioCodec(audio) : undefined,
    rtp: established ? securityState(audio, video) : undefined,
    renderer: rendererHealth(renderer),
    audio_renderer: renderingState(renderer?.audio, audio),
    video_renderer: renderingState(renderer?.video, video),
    audio_direction: established ? directionLabel(audio?.direction) : 'No established call',
    video_direction: established ? directionLabel(video?.direction) : 'No established call',
    hdmi: 'not reported', aec: readText(record(status.aec), 'delay_state') ?? undefined
  }
}

export function previewLabel(preview: JsonRecord | null): string {
  if (preview?.reason === 'not_configured') return 'Not configured'
  return readText(preview, 'state') ?? 'Not reported'
}
