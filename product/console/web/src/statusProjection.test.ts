import { describe, expect, it } from 'vitest'
import { connectionLabel, directionLabel, previewLabel, projectCallState, projectMedia } from './statusProjection'

describe('projectCallState', () => {
  it('keeps a completed hang-up distinct from missing status', () => {
    expect(projectCallState('terminated')).toEqual({
      active: false, emptyTitle: 'No active call',
      emptyDescription: 'The call has ended. You can join another meeting.'
    })
    expect(projectCallState('idle').emptyTitle).toBe('No active call')
    for (const state of [undefined, null, 'future_state']) {
      expect(projectCallState(state)).toMatchObject({ active: false, emptyTitle: 'Call state unavailable' })
    }
  })

  it('retains cancellation controls during retries and distinguishes terminal states', () => {
    for (const state of ['resolving', 'inviting', 'early', 'establishing_media',
      'established', 'terminating', 'failed', 'backing_off', 'connected', 'active', 'connecting']) {
      expect(projectCallState(state).active).toBe(true)
    }
    expect(projectCallState('stopped')).toMatchObject({ active: false, emptyTitle: 'Calling stopped' })
    expect(projectCallState('terminal_failure')).toMatchObject({ active: false, emptyTitle: 'Call failed' })
  })
})

describe('connectionLabel', () => {
  it('names established, idle, other and missing call states', () => {
    expect(connectionLabel({ call_state: 'established' })).toBe('SIP call established')
    expect(connectionLabel({ call_state: 'idle' })).toBe('Idle')
    expect(connectionLabel({ call_state: 'establishing_media' })).toBe('Call establishing media')
    expect(connectionLabel({})).toBe('Call state not reported')
    expect(connectionLabel(null)).toBe('Call state not reported')
  })
})

describe('directionLabel', () => {
  it('maps SDP directions and rejects anything else', () => {
    expect(directionLabel('sendrecv')).toBe('Send and receive')
    expect(directionLabel('inactive')).toBe('Inactive')
    expect(directionLabel('sideways')).toBe('Not reported')
    expect(directionLabel(undefined)).toBe('Not reported')
  })
})

describe('projectMedia', () => {
  const established = {
    call_state: 'established',
    audio: { direction: 'sendrecv', payload_type: 0, security: 'srtp' },
    video: { direction: 'sendrecv', h264_profile_level_id: '42e01f', security: 'srtp' },
    renderer: { healthy: true, audio: { rendering: true, fresh: true }, video: { rendering: true, fresh: false } },
    aec: { delay_state: 'uncalibrated' }
  }

  it('projects an established call', () => {
    expect(projectMedia(established)).toEqual({
      video_codec: 'H.264',
      audio_codec: 'G.711 μ-law',
      rtp: 'srtp',
      renderer: 'healthy',
      audio_renderer: 'healthy',
      video_renderer: 'not rendering',
      audio_direction: 'Send and receive',
      video_direction: 'Send and receive',
      hdmi: 'not reported',
      aec: 'uncalibrated'
    })
  })

  it('reports mixed protection and ignores inactive streams', () => {
    expect(projectMedia({ ...established, video: { ...established.video, security: 'rtp' } }).rtp).toBe('mixed srtp/rtp')
    expect(projectMedia({ ...established, video: { direction: 'inactive', security: 'rtp' } }).rtp).toBe('srtp')
    expect(projectMedia({ ...established, audio: { ...established.audio, security: 'plain' } }).rtp).toBeUndefined()
  })

  it('withholds codec and direction details without an established call', () => {
    const idle = projectMedia({ ...established, call_state: 'idle' })
    expect(idle.video_codec).toBeUndefined()
    expect(idle.audio_codec).toBeUndefined()
    expect(idle.rtp).toBeUndefined()
    expect(idle.audio_direction).toBe('No established call')
  })

  it('rejects a malformed H.264 profile and unknown audio payload types', () => {
    const media = projectMedia({
      ...established,
      audio: { direction: 'sendrecv', payload_type: 96 },
      video: { direction: 'sendrecv', h264_profile_level_id: '42e01' }
    })
    expect(media.video_codec).toBeUndefined()
    expect(media.audio_codec).toBeUndefined()
  })

  it('explains renderers for send-only and inactive streams', () => {
    const media = projectMedia({ ...established, audio: { direction: 'sendonly' }, video: { direction: 'inactive' } })
    expect(media.audio_renderer).toBe('Not expected (send only)')
    expect(media.video_renderer).toBe('Inactive stream')
  })
})

describe('previewLabel', () => {
  it('prefers the not-configured reason over the state', () => {
    expect(previewLabel({ reason: 'not_configured', state: 'idle' })).toBe('Not configured')
    expect(previewLabel({ state: 'streaming' })).toBe('streaming')
    expect(previewLabel(null)).toBe('Not reported')
  })
})
