import { useCallback, useState } from 'react'
import { api } from '../api'
import { useLivePolling, type ConnectionState } from '../hooks/useLivePolling'
import type { ApiError, Role } from '../types'
import { readText } from '../types'
import { Button, ErrorNotice } from './ui'
import { DeviceActivities } from './DeviceActivities'

export function ActivityBar({ role, onRoom, onConnection }: { role: Role; onRoom: () => void; onConnection: (state: ConnectionState) => void }) {
  const load = useCallback(async (signal: AbortSignal) => (await api.status(signal)).data, [])
  const { data, error, refresh } = useLivePolling({ load, onConnection })
  const [busy, setBusy] = useState(false)
  const [failure, setFailure] = useState<ApiError | null>(null)
  const state = readText(data, 'call_state') ?? 'unknown'
  const active = ['active', 'connected', 'connecting', 'resolving', 'inviting', 'early', 'establishing_media', 'established', 'terminating'].includes(state)
  const hangup = async () => {
    setBusy(true); setFailure(null)
    try { await api.hangup(); await refresh() }
    catch (cause) { setFailure(cause as ApiError) }
    finally { setBusy(false) }
  }
  return <aside className="activity-bar" aria-label="Room activities">
    <div className="activity-heading"><strong>Zoom · {error ? 'State unknown' : state}</strong><div className="inline-actions"><Button onClick={onRoom}>Open Room</Button>{active && role !== 'viewer' && <Button variant="danger" disabled={busy} onClick={() => void hangup()}>Hang up</Button>}</div></div>
    <ErrorNotice error={failure} />
    <div className="activity-bar__device"><DeviceActivities compact /></div>
  </aside>
}
