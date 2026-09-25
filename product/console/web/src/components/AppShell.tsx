import { Activity, CalendarDays, Library, BookUser, ChevronDown, CircleUserRound, MonitorPlay, Phone, Settings2, ShieldCheck, Video } from 'lucide-react'
import type { Page, Role } from '../types'
import { PAGE_LABELS } from '../types'
import { Freshness } from './ui'

const NAV_ITEMS: Array<{ page: Page; Icon: typeof Phone }> = [
  { page: 'calls', Icon: Phone },
  { page: 'directory', Icon: BookUser },
  { page: 'media', Icon: MonitorPlay },
  { page: 'settings', Icon: Settings2 },
  { page: 'diagnostics', Icon: Activity }
]

const ROOM_NAV: typeof NAV_ITEMS = [{ page: 'room', Icon: MonitorPlay }, { page: 'library', Icon: Library }, { page: 'schedule', Icon: CalendarDays }, { page: 'settings', Icon: Settings2 }]

const visibleNavigation = (role: Role) => role === 'admin'
  ? NAV_ITEMS
  : NAV_ITEMS.filter(({ page }) => page !== 'settings' && page !== 'diagnostics')

export function AppShell({ children, page, onPage, role, connection, onLogout, legacy = false }: {
  legacy?: boolean
  children: React.ReactNode
  page: Page
  onPage: (page: Page) => void
  role: Role
  connection: 'live' | 'stale' | 'offline' | 'loading'
  onLogout: () => void
}) {
  const secureOrigin = typeof window !== 'undefined' && window.location.protocol === 'https:'
  const navigation = legacy ? visibleNavigation(role) : ROOM_NAV
  return <div className="app-shell">
    <a href="#main-content" className="skip-link">Skip to content</a>
    <header className="topbar">
      <div className="brand"><Video aria-hidden="true" size={26} strokeWidth={1.8} /><span>LS200 Console</span></div>
      <div className="topbar__status"><Freshness state={connection} label={connection === 'live' ? (legacy ? 'Device online' : 'Zoom status current') : connection === 'loading' ? 'Checking device' : connection === 'stale' ? 'Zoom status stale' : 'Zoom status unavailable'} /><span className="security"><ShieldCheck aria-hidden="true" size={20} />{secureOrigin ? 'HTTPS session' : 'Development origin'}</span><span className="account"><CircleUserRound aria-hidden="true" size={25} />{role}<ChevronDown aria-hidden="true" size={17} /></span><button className="logout" onClick={onLogout}>Sign out</button></div>
    </header>
    <nav className="rail" aria-label="Main navigation">
      <div>{navigation.map(({ page: destination, Icon }) => <button key={destination} className={page === destination ? 'nav-item nav-item--active' : 'nav-item'} onClick={() => onPage(destination)} aria-current={page === destination ? 'page' : undefined}><Icon aria-hidden="true" size={25} strokeWidth={1.7} /><span>{PAGE_LABELS[destination]}</span></button>)}</div>
      <p className="rail__footer">LS200 Systems</p>
    </nav>
    <main id="main-content" className="content" tabIndex={-1}>{children}</main>
    <nav className="bottom-nav" aria-label="Main navigation">{navigation.map(({ page: destination, Icon }) => <button key={destination} className={page === destination ? 'bottom-nav__item bottom-nav__item--active' : 'bottom-nav__item'} onClick={() => onPage(destination)} aria-current={page === destination ? 'page' : undefined}><Icon aria-hidden="true" size={25} strokeWidth={1.7} /><span>{PAGE_LABELS[destination]}</span></button>)}</nav>
  </div>
}
