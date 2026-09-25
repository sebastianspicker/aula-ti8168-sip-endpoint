import { useEffect, useRef, useState } from 'react'
import { api } from '../api'
import { isRecord, readNumber, type ApiError, type JsonRecord } from '../types'
import { Button, ErrorNotice, Notice, ValueRow } from './ui'

const FIELDS = [['queue_depth', 'Queue depth'], ['packet_drops', 'Packet drops'],
  ['access_unit_drops', 'Access unit drops'], ['backend_drops', 'Backend drops'],
  ['backend_restarts', 'Backend restarts']] as const

export function DiagnosticsMetrics() {
  const [data, setData] = useState<JsonRecord | null>(null)
  const [error, setError] = useState<ApiError | null>(null)
  const [busy, setBusy] = useState(false)
  const current = useRef<AbortController | null>(null)
  useEffect(() => () => { current.current?.abort(); current.current = null }, [])
  const load = async () => {
    current.current?.abort()
    const controller = new AbortController()
    current.current = controller
    setBusy(true); setError(null)
    try {
      const result = await api.metrics(controller.signal)
      if (current.current === controller) setData(result.data)
    } catch (cause) {
      if (current.current === controller && !controller.signal.aborted) setError(cause as ApiError)
    } finally {
      if (current.current === controller) setBusy(false)
    }
  }
  return <section className="settings-section" aria-labelledby="metrics-title">
    <h2 id="metrics-title">Runtime metrics</h2>
    <p>Snapshots are fetched only on request. Stream counters reset with the media session; poll counters reset with the process. Shared backend counters appear on both streams. Audio access unit drops remain zero.</p>
    <Button onClick={() => void load()} disabled={busy}>{busy ? 'Loading metrics…' : 'Load runtime metrics'}</Button>
    <ErrorNotice error={error} />
    {error && <Notice tone="info">Runtime metrics are unavailable. Other diagnostics remain available; older daemons may not support this snapshot.</Notice>}
    {data && <MetricsSnapshot data={data} />}
    {!data && !busy && !error && <Notice tone="info">No runtime metrics requested.</Notice>}
    {data && error && <Notice tone="info">The displayed snapshot is stale.</Notice>}
  </section>
}

function MetricsSnapshot({ data }: { data: JsonRecord }) {
  const streams = Array.isArray(data?.streams) ? data.streams.filter(isRecord) : []
  const poll = isRecord(data?.poll) ? data.poll : null
  return <div aria-label="Runtime metrics snapshot">{streams.map((stream, index) => <div key={index}>
      <h3>{stream.kind === 'audio' ? 'Audio' : 'Video'}</h3>
      {FIELDS.map(([key, label]) => <ValueRow key={key} label={label} value={readNumber(stream, key)?.toString() ?? 'Not reported'} />)}
    </div>)}<ValueRow label="Late polls" value={readNumber(poll, 'late_count')?.toString() ?? 'Not reported'} /><ValueRow label="Maximum poll lateness (ms)" value={readNumber(poll, 'max_lateness_ms')?.toString() ?? 'Not reported'} /></div>
}
