import { useEffect, useState, type RefObject } from 'react'
import { api } from '../api'

export const PREVIEW_STREAM_PATH = '/zoom/api/v1/media/preview.flv'

export type PreviewPlayerState = 'idle' | 'loading' | 'playing' | 'unsupported' | 'error'

export function usePreviewPlayer(
  video: RefObject<HTMLVideoElement | null>,
  streamPath: string | null,
  advertisedState: string
): PreviewPlayerState {
  const [state, setState] = useState<PreviewPlayerState>('idle')

  useEffect(() => {
    if (streamPath === null || !['idle', 'live'].includes(advertisedState)) {
      setState('idle')
      return
    }
    if (streamPath !== PREVIEW_STREAM_PATH || video.current === null) {
      setState('error')
      return
    }
    const headers = api.previewStreamHeaders()
    if (headers === null) {
      setState('error')
      return
    }

    let active = true
    let destroy: (() => void) | null = null
    setState('loading')
    void import('mpegts.js').then(({ default: mpegts }) => {
      if (!active || video.current === null) return
      const features = mpegts.getFeatureList()
      if (!mpegts.isSupported() || !features.mseLivePlayback ||
          !features.nativeMP4H264Playback) {
        setState('unsupported')
        return
      }
      const player = mpegts.createPlayer({
        type: 'flv', isLive: true, hasAudio: false, hasVideo: true,
        url: PREVIEW_STREAM_PATH, withCredentials: true
      }, {
        headers,
        enableWorker: false,
        enableWorkerForMSE: false,
        enableStashBuffer: false,
        lazyLoad: false,
        autoCleanupSourceBuffer: true,
        autoCleanupMaxBackwardDuration: 12,
        autoCleanupMinBackwardDuration: 4,
        liveSync: true,
        liveSyncMaxLatency: 2,
        liveSyncTargetLatency: 1,
        referrerPolicy: 'no-referrer'
      })
      const onError = () => { if (active) setState('error') }
      player.on(mpegts.Events.ERROR, onError)
      player.attachMediaElement(video.current)
      player.load()
      void Promise.resolve(player.play()).then(() => {
        if (active) setState('playing')
      }).catch(onError)
      destroy = () => {
        player.off(mpegts.Events.ERROR, onError)
        player.pause()
        player.unload()
        player.detachMediaElement()
        player.destroy()
      }
    }).catch(() => { if (active) setState('error') })

    return () => {
      active = false
      destroy?.()
    }
  }, [advertisedState, streamPath, video])

  return state
}
