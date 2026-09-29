import { useState } from 'react'
import { SettingsScreen } from './SettingsScreen'
import { DiagnosticsScreen } from './DiagnosticsScreen'
import { Notice } from '../components/ui'
import { OemCredentials } from '../components/OemCredentials'
import type { Role } from '../types'
import type { ConnectionState } from '../hooks/useLivePolling'

const GROUPS = ['Calling', 'Accounts', 'Diagnostics'] as const
type Group = typeof GROUPS[number]

export function UnifiedSettingsScreen({ role, onConnection }: { role: Role; onConnection: (state: ConnectionState) => void }) {
  const [group, setGroup] = useState<Group>('Calling')
  if (role !== 'admin') return <section><h1>Settings</h1><Notice tone="info">An administrator account is required to configure the console.</Notice></section>
  return <section aria-labelledby="unified-settings-title">
    <div className="page-heading"><div><h1 id="unified-settings-title">Settings</h1><p>Calling, accounts, and diagnostics.</p></div></div>
    <div className="settings-groups" role="group" aria-label="Settings groups">{GROUPS.map(item => <button className={item === group ? 'button button--primary' : 'button'} key={item} aria-pressed={item === group} onClick={() => setGroup(item)}>{item}</button>)}</div>
    {group !== 'Diagnostics' && <h2 className="section-gap">{group}</h2>}
    {group === 'Calling' && <SettingsScreen section="sip" role={role} onConnection={onConnection} />}
    {group === 'Accounts' && <><OemCredentials /><SettingsScreen section="accounts" role={role} onConnection={onConnection} /></>}
    {group === 'Diagnostics' && <DiagnosticsScreen onConnection={onConnection} />}
    <p className="section-gap"><a href="#legacy" onClick={() => window.location.assign(`${window.location.pathname}#legacy`)}>Previous Zoom console</a></p>
  </section>
}
