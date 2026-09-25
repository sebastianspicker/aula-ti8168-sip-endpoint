import { useCallback, useEffect, useRef, useState } from 'react'
import type { ApiError } from '../types'
import { PollingLifecycle } from './pollingLifecycle'

export type ConnectionState = 'live' | 'stale' | 'offline'

type LivePollingOptions<T> = {
  load: (signal: AbortSignal) => Promise<T>
  onConnection: (state: ConnectionState) => void
  intervalMs?: number
}

const DEFAULT_INTERVAL_MS = 5_000

export function useLivePolling<T>({ load, onConnection, intervalMs = DEFAULT_INTERVAL_MS }: LivePollingOptions<T>) {
  const [data, setData] = useState<T | null>(null)
  const [error, setError] = useState<ApiError | null>(null)
  const dataRef = useRef<T | null>(null)
  const loadRef = useRef(load)
  const onConnectionRef = useRef(onConnection)
  const lifecycleRef = useRef<PollingLifecycle<T> | null>(null)
  loadRef.current = load
  onConnectionRef.current = onConnection

  const refresh = useCallback(() => lifecycleRef.current?.refresh() ?? Promise.resolve(), [])

  useEffect(() => {
    const lifecycle = new PollingLifecycle({
      load: signal => loadRef.current(signal),
      success: nextData => {
        dataRef.current = nextData
        setData(nextData)
        setError(null)
        onConnectionRef.current('live')
      },
      failure: cause => {
        if ((cause as ApiError)?.code === 'REQUEST_ABORTED') return
        setError(cause as ApiError)
        onConnectionRef.current(dataRef.current === null ? 'offline' : 'stale')
      }
    }, intervalMs)
    lifecycleRef.current = lifecycle
    lifecycle.start()
    return () => {
      lifecycle.stop()
      if (lifecycleRef.current === lifecycle) lifecycleRef.current = null
    }
  }, [intervalMs])

  return { data, error, refresh }
}
