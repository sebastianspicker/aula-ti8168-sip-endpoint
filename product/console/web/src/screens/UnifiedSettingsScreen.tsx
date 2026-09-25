import { useState } from 'react'
import { SettingsScreen } from './SettingsScreen'
import { DiagnosticsScreen } from './DiagnosticsScreen'
import { Notice } from '../components/ui'
import { OemCredentials } from '../components/OemCredentials'
import type { Role } from '../types'
import type { ConnectionState } from '../hooks/useLivePolling'

const GROUPS = ['Device', 'Picture & sound', 'Outputs', 'Network', 'Storage', 'Accounts', 'Maintenance'] as const
type Group = typeof GROUPS[number]
const ADVANCED: Partial<Record<Group, string>> = {
  Device: 'Timezone, date and time, language, front panel, external devices, and serial controls.',
  'Picture & sound': 'Sources, camera configuration, audio routing, crop, overlays, backgrounds, and themes.',
  Outputs: 'Encoder profiles, display layout, streaming destinations, and output resolution.',
  Network: 'Interfaces, gateway, DNS, Wi-Fi, and reconnect after applying changes.',
  Storage: 'Local storage, NAS, upload destinations, cloud accounts, and transfer status.',
  Maintenance: 'Configuration backup and restore, firmware, restart, and factory reset.'
}

export function UnifiedSettingsScreen({ role, onConnection }: { role: Role; onConnection: (state: ConnectionState) => void }) {
  const [group, setGroup] = useState<Group>('Device')
  if (role !== 'admin') return <section><h1>Settings</h1><Notice tone="info">An administrator account is required to configure the device.</Notice></section>
  return <section aria-labelledby="unified-settings-title">
    <div className="page-heading"><div><h1 id="unified-settings-title">Settings</h1><p>Device configuration and administration.</p></div></div>
    <div className="settings-groups" role="group" aria-label="Settings groups">{GROUPS.map(item => <button className={item === group ? 'button button--primary' : 'button'} key={item} aria-pressed={item === group} onClick={() => setGroup(item)}>{item}</button>)}</div>
    <h2 className="section-gap">{group}</h2>
    {group === 'Accounts' ? <><OemCredentials /><SettingsScreen section="accounts" role={role} onConnection={onConnection} /></> : <>
      <Notice tone="info">Device configuration controls in this group are not yet available in the console.</Notice>
      <details><summary>Advanced</summary><p>{ADVANCED[group]}</p></details>
      {group === 'Outputs' && <SettingsScreen section="sip" role={role} onConnection={onConnection} />}
      {group === 'Maintenance' && <DiagnosticsScreen onConnection={onConnection} />}
    </>}
    <p className="section-gap"><a href="#legacy" onClick={() => window.location.assign(`${window.location.pathname}#legacy`)}>Previous Zoom console</a></p>
  </section>
}
