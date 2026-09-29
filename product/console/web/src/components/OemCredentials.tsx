import { useCallback } from 'react'
import { api, type OemCredentialsStatus } from '../api'
import { useLivePolling } from '../hooks/useLivePolling'
import { Button, ErrorNotice, Notice, ValueRow } from './ui'

const ignoreConnection = () => undefined

function StoredStatus({ data }: { data: OemCredentialsStatus | null }) {
  return <>
    <ValueRow label="Stored credentials" value={data ? data.credentials_present ? 'Present' : 'None stored' : 'Unavailable'} />
    <ValueRow label="Stored revision" value={data ? String(data.revision) : 'Unavailable'} />
    <ValueRow label="Device connection" value="Unavailable" />
  </>
}

export function OemCredentials() {
  const load = useCallback(async (signal: AbortSignal) => (await api.oemCredentials(signal)).data, [])
  const { data, error, refresh } = useLivePolling({ load, onConnection: ignoreConnection })
  return <section className="settings-section oem-credentials section-gap" aria-labelledby="oem-credentials-title">
    <h3 id="oem-credentials-title">Device connection</h3>
    <Notice tone="info">Device connection is unavailable. Stored credential status is shown for reference; the console does not contact firmware.</Notice>
    <ErrorNotice error={error} />
    <StoredStatus data={data} />
    <Button onClick={() => void refresh()}>Reload stored status</Button>
  </section>
}
