import { Download, RefreshCw, ShieldAlert } from 'lucide-react'
import { useCallback, useState } from 'react'
import { api } from '../api'
import { CallEvidence } from '../components/CallEvidence'
import { DiagnosticsMetrics } from '../components/DiagnosticsMetrics'
import type { ApiError, JsonRecord } from '../types'
import { isRecord, readText } from '../types'
import { Button, EmptyState, ErrorNotice, LoadingRows, Notice, ValueRow } from '../components/ui'
import { useLivePolling } from '../hooks/useLivePolling'

type DiagnosticsData = { diagnostics: JsonRecord; events: JsonRecord; status: JsonRecord | null }

const BOUNDED_CHECKS = [
  ['configuration', 'Configuration'],
  ['sip_control', 'SIP control'],
  ['media_readiness', 'Media readiness'],
  ['tls_profile_policy', 'TLS profile policy'],
  ['renderer_truth', 'Renderer truth']
] as const

function readinessLabel(value: unknown) {
  return value === 'ready' ? 'ready' : value === 'not_ready' ? 'not ready' : value === 'unavailable' ? 'unavailable' : 'not reported'
}

export function DiagnosticsScreen({ onConnection }: { onConnection: (state: 'live' | 'stale' | 'offline') => void }) {
  const [actionError, setActionError] = useState<ApiError | null>(null)
  const [message, setMessage] = useState<string | null>(null)
  const [testResult, setTestResult] = useState<JsonRecord | null>(null)
  const [busy, setBusy] = useState(false)
  const loadDiagnostics = useCallback(async (signal: AbortSignal): Promise<DiagnosticsData> => {
    const [diagnostics, events, status] = await Promise.all([api.diagnostics(signal), api.events(signal), api.status(signal).catch(() => null)])
    return { diagnostics: diagnostics.data, events: events.data, status: status?.data ?? null }
  }, [])
  const { data, error, refresh } = useLivePolling({ load: loadDiagnostics, onConnection })
  const { diagnostics, events, status } = data ?? { diagnostics: null, events: null, status: null }
  const exportId = readText(diagnostics, 'export_id')
  const download = async () => {
    if (!exportId) return
    setBusy(true); setActionError(null)
    try {
      const result = await api.exportDiagnostics(exportId)
      const url = URL.createObjectURL(new Blob([
        `${JSON.stringify(result.data, null, 2)}\n`
      ], { type: 'application/json' }))
      const link = document.createElement('a')
      link.href = url
      link.download = `aula-diagnostics-${exportId}.json`
      link.click()
      URL.revokeObjectURL(url)
      setMessage('Redacted export downloaded.')
    } catch (cause) {
      setActionError(cause as ApiError)
    } finally {
      setBusy(false)
    }
  }
  const runBoundedTests = async () => {
    setBusy(true); setActionError(null); setMessage(null)
    try {
      const result = await api.runDiagnostics()
      setTestResult(result.data)
      await refresh()
    } catch (cause) {
      setActionError(cause as ApiError)
    } finally {
      setBusy(false)
    }
  }
  return <section aria-labelledby="diagnostics-title">
    <div className="page-heading"><div><h1 id="diagnostics-title">Diagnostics</h1><p>Support evidence is device-generated and redacted at the boundary.</p></div><Button onClick={() => void refresh()}><RefreshCw aria-hidden="true" size={17} /> Refresh</Button></div>
    <ErrorNotice error={actionError ?? error} />
    {message && <Notice tone="success">{message}</Notice>}
    {!diagnostics || !events ? <LoadingRows count={7} /> : <DiagnosticsDetails status={status} stale={error !== null} diagnostics={diagnostics} events={events} testResult={testResult} busy={busy} exportId={exportId} onDownload={download} onRunBoundedTests={runBoundedTests} />}
  </section>
}

function DiagnosticsDetails({ status, stale, diagnostics, events, testResult, busy, exportId, onDownload, onRunBoundedTests }: {
  status: JsonRecord | null; stale: boolean; diagnostics: JsonRecord; events: JsonRecord; testResult: JsonRecord | null; busy: boolean; exportId: string | null; onDownload: () => Promise<void>; onRunBoundedTests: () => Promise<void>
}) {
  return <div className="diagnostics-layout">
    <section className="settings-section">
      <h2>Current connection</h2><CallEvidence status={status} stale={stale} /><BoundedTests diagnostics={diagnostics} testResult={testResult} busy={busy} onRunBoundedTests={onRunBoundedTests} />
      <DiagnosticsProperties diagnostics={diagnostics} />
      <RedactedExport exportId={exportId} busy={busy} onDownload={onDownload} />
    </section>
    <div><DiagnosticsMetrics /><EventTimeline events={events} /></div>
  </div>
}

function BoundedTests({ diagnostics, testResult, busy, onRunBoundedTests }: {
  diagnostics: JsonRecord; testResult: JsonRecord | null; busy: boolean; onRunBoundedTests: () => Promise<void>
}) {
  const checks = isRecord(testResult?.checks) ? testResult.checks : null
  return <><h2 className="section-gap">Tests</h2><ValueRow label="Last test" value={testResult ? 'Completed in this browser session' : readText(diagnostics, 'last_test') ?? 'Not reported'} /><ValueRow label="Status availability" value={readText(diagnostics, 'result') ?? 'Not reported'} /><Notice tone="info">Bounded checks are separate from current call state. Unavailable media or renderer checks do not establish a failed SIP connection or successful remote reception.</Notice><Button onClick={() => void onRunBoundedTests()} disabled={busy}>Run bounded tests</Button>{testResult ? <div aria-label="Bounded test results"><ValueRow label="Bounded checks overall" value={readinessLabel(testResult.state)} />{BOUNDED_CHECKS.map(([key, label]) => <ValueRow key={key} label={label} value={readinessLabel(isRecord(checks?.[key]) ? checks[key].state : null)} />)}</div> : <Notice tone="info">Run bounded tests to retrieve the current readiness report.</Notice>}</>
}

function releaseVersion(diagnostics: JsonRecord, key: string) {
  const version = readText(diagnostics, key)
  return version && version !== '1' ? version : 'Release version not reported'
}

function DiagnosticsProperties({ diagnostics }: { diagnostics: JsonRecord }) {
  return <><h2 className="section-gap">Versions and SBOM</h2><ValueRow label="Console" value={releaseVersion(diagnostics, 'console_version')} /><ValueRow label="SIP service" value={releaseVersion(diagnostics, 'sip_version')} /><ValueRow label="SBOM" value={readText(diagnostics, 'sbom') ?? 'Not reported'} /><h2 className="section-gap">Resources</h2><ValueRow label="CPU" value={readText(diagnostics, 'cpu') ?? 'Not reported'} /><ValueRow label="Memory" value={readText(diagnostics, 'memory') ?? 'Not reported'} /><ValueRow label="Storage" value={readText(diagnostics, 'storage') ?? 'Not reported'} /></>
}

function RedactedExport({ exportId, busy, onDownload }: { exportId: string | null; busy: boolean; onDownload: () => Promise<void> }) {
  return <><h2 className="section-gap">Redacted export</h2><Notice tone="info"><ShieldAlert aria-hidden="true" size={18} /> The console cannot request an export until the device reports an opaque export ID.</Notice><Button onClick={() => void onDownload()} disabled={!exportId || busy}><Download aria-hidden="true" size={18} /> Download redacted export</Button></>
}

function EventTimeline({ events }: { events: JsonRecord }) {
  const eventRows = Array.isArray(events.events) ? events.events.filter(isRecord) : []
  return <section className="settings-section"><h2>Bounded event timeline</h2>{eventRows.length === 0 ? <EmptyState title="No events reported">The device did not return recent event metadata.</EmptyState> : <ol className="event-list">{eventRows.map((event, index) => <li key={`${readText(event, 'id') ?? 'event'}-${index}`}><time>{readText(event, 'at') ?? 'Time not reported'}</time><div><strong>{readText(event, 'type') ?? 'Device event'}</strong><span>{readText(event, 'message') ?? 'Details not reported'}</span></div></li>)}</ol>}</section>
}
