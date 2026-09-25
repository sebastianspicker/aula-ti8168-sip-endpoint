import { BookUser } from 'lucide-react'
import { useState } from 'react'
import { CallsScreen } from './CallsScreen'
import { DirectoryScreen } from './DirectoryScreen'
import { RoomPreview } from '../components/RoomPreview'
import { DeviceActivities } from '../components/DeviceActivities'
import { Button, Notice } from '../components/ui'
import type { Role } from '../types'
import type { ConnectionState } from '../hooks/useLivePolling'

export function RoomScreen({ role, onConnection }: { role: Role; onConnection: (state: ConnectionState) => void }) {
  const [directory, setDirectory] = useState(false)
  return <section aria-labelledby="room-title">
    <div className="page-heading"><div><h1 id="room-title">Room</h1><p>Preview and room activities.</p></div><Button onClick={() => setDirectory(value => !value)} aria-expanded={directory}><BookUser aria-hidden="true" size={18} />Meeting directory</Button></div>
    {directory && <DirectoryScreen role={role} onConnection={onConnection} />}
    <div className="room-layout"><div>
      <RoomPreview />
      <p className="honesty-note">Local preview is independent of the meeting. It does not verify what remote participants receive.</p>
      <details className="room-picture"><summary>Picture &amp; sound</summary>
        <Notice tone="info">Source, local layout, input sound, and camera controls are not yet available in this console.</Notice>
        <p>Meeting mute affects outbound meeting audio. Device input mute and local picture layout are separate settings.</p>
        <details><summary>Advanced</summary><p>Camera presets, audio routing, crop, overlays, backgrounds, and themes are not yet available here.</p></details>
      </details>
    </div><div className="room-activities">
      <CallsScreen role={role} onConnection={onConnection} embedded />
      <DeviceActivities />
    </div></div>
  </section>
}
