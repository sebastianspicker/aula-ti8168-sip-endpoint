import { useCallback } from 'react'
import { api } from '../api'
import { useLivePolling } from '../hooks/useLivePolling'
import { readText } from '../types'
import { Notice } from './ui'

const ignoreConnection = () => undefined
const STATES: Record<string, string> = { active: 'Active', inactive: 'Stopped', paused: 'Paused', unknown: 'State unknown' }

export function DeviceActivities({ compact = false }: { compact?: boolean }) {
  const load = useCallback(async (signal: AbortSignal) => (await api.deviceStatus(signal)).data, [])
  const { data, error } = useLivePolling({ load, onConnection: ignoreConnection })
  return <>{(['recording', 'streaming'] as const).map(activity => {
    const state = readText(data, activity) ?? 'unknown'
    return <section className="room-activity" key={activity} aria-label={activity === 'recording' ? 'Recording activity' : 'Streaming activity'}>
      <div className="activity-heading"><h2>{activity === 'recording' ? 'Recording' : 'Streaming'}</h2><span>{error ? 'State unknown' : STATES[state] ?? 'State unknown'}</span></div>
      {(!compact || state === 'active' || state === 'paused') && <p>{state === 'active' || state === 'paused' ? 'Device controls are not yet available here. Use the existing device workflow to stop this activity.' : 'Device controls are not yet available in this console.'}</p>}
      {error && !compact && <Notice tone="warning">Device state could not be verified.</Notice>}
    </section>
  })}</>
}
