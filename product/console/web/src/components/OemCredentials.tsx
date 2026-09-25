import { useCallback, useState } from 'react'
import { api, type OemCredentialsStatus } from '../api'
import type { ApiError } from '../types'
import { useLivePolling } from '../hooks/useLivePolling'
import { Button, ErrorNotice, Notice, ValueRow } from './ui'

const ignoreConnection = () => undefined
const CONNECTIONS: Record<string, string> = {
  connected: 'Connected', unknown: 'Not verified', unconfigured: 'Credentials required',
  authentication_failed: 'Login failed', backoff: 'Waiting before the next login attempt'
}

function ConnectionSummary({ data, error, revision }: { data: OemCredentialsStatus | null; error: ApiError | null; revision: number | null }) {
  return <>
    <ValueRow label="Credentials" value={data ? data.credentials_present ? 'Configured' : 'Not configured' : 'Unavailable'} />
    <ValueRow label="Connection" value={error ? 'State unknown' : CONNECTIONS[data?.connection ?? 'unknown']} />
    {revision !== null && data && revision !== data.revision && <Notice tone="warning">Device credentials changed while you were editing. Reload before saving.</Notice>}
  </>
}

function invalidDraft(username: string, password: string) {
  return !username || !password || username.includes(':') || password.includes(':')
}

export function OemCredentials() {
  const load = useCallback(async (signal: AbortSignal) => (await api.oemCredentials(signal)).data, [])
  const { data, error, refresh } = useLivePolling({ load, onConnection: ignoreConnection })
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [revision, setRevision] = useState<number | null>(null)
  const [busy, setBusy] = useState(false)
  const [failure, setFailure] = useState<ApiError | null>(null)
  const [saved, setSaved] = useState(false)
  const edit = (field: 'username' | 'password', value: string) => {
    if (revision === null && data) setRevision(data.revision)
    setSaved(false)
    if (field === 'username') setUsername(value)
    else setPassword(value)
  }
  const save = async () => {
    if (revision === null || busy) return
    setBusy(true); setFailure(null); setSaved(false)
    try {
      await api.saveOemCredentials(username, password, revision)
      setUsername(''); setPassword(''); setRevision(null); setSaved(true)
      await refresh()
    } catch (cause) { setFailure(cause as ApiError) }
    finally { setBusy(false) }
  }
  const reload = async () => {
    setUsername(''); setPassword(''); setRevision(null); setFailure(null); setSaved(false)
    await refresh()
  }
  return <section className="settings-section oem-credentials section-gap" aria-labelledby="oem-credentials-title">
    <h3 id="oem-credentials-title">Device connection</h3>
    <p>Save the device’s OEM account credentials. They are stored on the server and are never returned to the console.</p>
    <ErrorNotice error={error ?? failure} />
    <ConnectionSummary data={data} error={error} revision={revision} />
    {saved && <Notice tone="success">Credentials saved. Connection status updates when device status is checked.</Notice>}
    <form onSubmit={event => { event.preventDefault(); void save() }}>
      <fieldset className="form-grid" aria-label="OEM credentials" disabled={busy || !data || !!error}>
        <label>OEM username<input autoComplete="off" maxLength={64} required value={username} onChange={event => edit('username', event.target.value)} /></label>
        <label>OEM password<input type="password" autoComplete="new-password" maxLength={128} required value={password} onChange={event => edit('password', event.target.value)} /></label>
        <p>Colons are not supported by the device login protocol.</p>
        <Button type="submit" variant="primary" disabled={invalidDraft(username, password) || revision !== data?.revision}>Save OEM credentials</Button>
      </fieldset>
    </form>
    <Button disabled={busy} onClick={() => void reload()}>Reload device connection</Button>
  </section>
}
