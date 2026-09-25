import { useCallback, useEffect, useState } from 'react'
import { api } from './api'
import { AppShell } from './components/AppShell'
import { Auth } from './components/Auth'
import { CallsScreen } from './screens/CallsScreen'
import { DiagnosticsScreen } from './screens/DiagnosticsScreen'
import { DirectoryScreen } from './screens/DirectoryScreen'
import { MediaScreen } from './screens/MediaScreen'
import { RoomScreen } from './screens/RoomScreen'
import { UnifiedSettingsScreen } from './screens/UnifiedSettingsScreen'
import { CollectionScreen } from './screens/CollectionScreen'
import { ActivityBar } from './components/ActivityBar'
import { SettingsScreen } from './screens/SettingsScreen'
import type { Page, Role } from './types'

export default function App() {
  const [role, setRole] = useState<Role | null>(null)
  const [checkingSession, setCheckingSession] = useState(true)
  const [legacy, setLegacy] = useState(() => window.location.hash === '#legacy')
  useEffect(() => {
    const change = () => { const old = window.location.hash === '#legacy'; setLegacy(old); setPage(old ? 'calls' : 'room') }
    window.addEventListener('hashchange', change)
    return () => window.removeEventListener('hashchange', change)
  }, [])
  const [page, setPage] = useState<Page>(() => legacy ? 'calls' : 'room')
  const [connection, setConnection] = useState<'live' | 'stale' | 'offline' | 'loading'>('loading')
  useEffect(() => { let active = true; void api.session().then(result => { if (active) setRole(result.data.role) }).catch(() => undefined).finally(() => { if (active) setCheckingSession(false) }); return () => { active = false } }, [])
  const logout = useCallback(() => { void api.logout().catch(() => undefined).finally(() => { setRole(null); setPage(legacy ? 'calls' : 'room'); setConnection('loading') }) }, [legacy])
  const setPageAndFocus = useCallback((next: Page) => { setPage(next); requestAnimationFrame(() => document.querySelector<HTMLElement>('#main-content')?.focus()) }, [])
  const onConnection = useCallback((next: 'live' | 'stale' | 'offline') => { setConnection(next) }, [])
  if (checkingSession) return <div className="session-loading" role="status">Checking protected device session…</div>
  if (!role) return <Auth onAuthenticated={setRole} />
  const shared = { onConnection }
  const screen = page === 'room' ? <RoomScreen role={role} {...shared} /> : page === 'library' || page === 'schedule' ? <CollectionScreen key={page} page={page} /> : page === 'settings' && !legacy ? <UnifiedSettingsScreen role={role} {...shared} /> : page === 'calls' ? <CallsScreen role={role} {...shared} /> : page === 'directory' ? <DirectoryScreen role={role} {...shared} /> : page === 'media' ? <MediaScreen {...shared} /> : page === 'settings' ? <SettingsScreen role={role} {...shared} /> : <DiagnosticsScreen {...shared} />
  return <AppShell legacy={legacy} page={page} onPage={setPageAndFocus} role={role} connection={connection} onLogout={logout}>{!legacy && page !== 'room' && <ActivityBar onConnection={onConnection} role={role} onRoom={() => setPageAndFocus('room')} />}{screen}</AppShell>
}
