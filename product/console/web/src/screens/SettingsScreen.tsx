import { KeyRound, RefreshCw, ShieldCheck, Trash2, UserPlus } from 'lucide-react'
import { useCallback, useEffect, useState } from 'react'
import { api } from '../api'
import type { ApiError, JsonRecord, Role, SettingsRequest } from '../types'
import { isRecord, readNumber, readText } from '../types'
import { Button, EmptyState, ErrorNotice, LoadingRows, Notice, ValueRow } from '../components/ui'
import { useLivePolling } from '../hooks/useLivePolling'

type SettingsData = { settings: JsonRecord; users: JsonRecord }
type AccountDraft = { username: string; password: string; role: Role }

const EMPTY_ACCOUNT: AccountDraft = { username: '', password: '', role: 'viewer' }

export function SettingsScreen({ role, onConnection, section = 'all' }: {
  section?: 'all' | 'sip' | 'accounts'
  role: Role
  onConnection: (state: 'live' | 'stale' | 'offline') => void
}) {
  const [actionError, setActionError] = useState<ApiError | null>(null)
  const [message, setMessage] = useState<string | null>(null)
  const [credential, setCredential] = useState({ username: '', password: '' })
  const [account, setAccount] = useState<AccountDraft>(EMPTY_ACCOUNT)
  const [settingsDraft, setSettingsDraft] = useState<Pick<SettingsRequest, 'profile' | 'media'>>({ profile: 'zoom_direct', media: 'managed' })
  const [pendingDelete, setPendingDelete] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)
  const loadSettings = useCallback(async (signal: AbortSignal): Promise<SettingsData> => {
    const [settings, users] = await Promise.all([api.settings(signal), api.users(signal)])
    return { settings: settings.data, users: users.data }
  }, [])
  const { data, error, refresh } = useLivePolling({ load: loadSettings, onConnection })
  const settings = data?.settings ?? null
  const users = data?.users ?? null
  const editable = role === 'admin'
  const settingsRevision = readNumber(settings, 'revision') ?? 1
  const accountRevision = readNumber(users, 'revision') ?? 0
  const accountRows = users && Array.isArray(users.users) ? users.users.filter(isRecord) : []
  const reportedTls = readText(settings, 'tls')
  const reportedProfile = readText(settings, 'profile')
  const reportedMedia = readText(settings, 'media')
  const tlsPolicy = reportedTls === 'required' ? 'Required' :
    reportedTls === 'not_required' ? 'Not required' : 'Not reported'

  useEffect(() => {
    setSettingsDraft({
      profile: reportedProfile === 'zoom_proxy' || reportedProfile === 'private_lab' ? reportedProfile : 'zoom_direct',
      media: reportedMedia === 'disabled' ? 'disabled' : 'managed'
    })
  }, [reportedMedia, reportedProfile, settingsRevision])

  const perform = async (operation: () => Promise<unknown>, success: string) => {
    setBusy(true); setActionError(null); setMessage(null)
    try { await operation(); setMessage(success); await refresh(); return true }
    catch (cause) { setActionError(cause as ApiError); return false }
    finally { setBusy(false) }
  }
  const replaceCredentials = async () => {
    if (await perform(() => api.replaceCredentials(credential.username, credential.password), 'SIP credential replacement was accepted.')) setCredential({ username: '', password: '' })
  }
  const saveDeviceSettings = async () => {
    await perform(
      () => api.saveSettings({ revision: settingsRevision, ...settingsDraft }),
      'Device policy was applied.'
    )
  }
  const saveAccount = async () => {
    if (await perform(() => api.saveUser(account.username, account.password, account.role, accountRevision), 'Account state was saved.')) setAccount(EMPTY_ACCOUNT)
  }
  const deleteAccount = async (username: string) => {
    if (await perform(() => api.deleteUser(username, accountRevision), `Account ${username} was removed.`)) setPendingDelete(null)
  }

  return <section aria-labelledby="settings-title">
    {section === 'all' && <div className="page-heading"><div><h1 id="settings-title">Settings</h1><p>Live device policy, accounts, and write-only SIP credentials.</p></div><Button onClick={() => void refresh()}><RefreshCw aria-hidden="true" size={17} /> Refresh</Button></div>}
    <ErrorNotice error={actionError ?? error} />
    {message && <Notice tone="success">{message}</Notice>}
    {!settings || !users ? <LoadingRows count={8} /> : <div className="settings-layout">
      {section !== 'accounts' && <section className="settings-section">
        <h2>SIP profile</h2>
        <Notice tone="info">Changes are revision-bound and apply only while no call is active. SIP destinations and credentials are never returned to this console.</Notice>
        <label>Connection policy<select value={settingsDraft.profile} onChange={event => setSettingsDraft(value => ({ ...value, profile: event.target.value as SettingsRequest['profile'] }))} disabled={!editable || busy}><option value="zoom_direct">Zoom direct</option><option value="zoom_proxy">Zoom proxy</option><option value="private_lab">Private lab</option></select><small>Proxy mode requires separately provisioned write-only credentials.</small></label>
        <label>Media policy<select value={settingsDraft.media} onChange={event => setSettingsDraft(value => ({ ...value, media: event.target.value as SettingsRequest['media'] }))} disabled={!editable || busy}><option value="managed">Managed</option><option value="disabled">Disabled</option></select></label>
        <ValueRow label="TLS policy" value={tlsPolicy} />
        <Button type="button" onClick={() => void saveDeviceSettings()} disabled={!editable || busy || settingsRevision < 1}>Apply policy</Button>
        <ValueRow label="Registration" value={readText(settings, 'registration') ?? 'Not reported'} />
        <h2 className="section-gap">Write-only SIP credentials</h2>
        <div className="credential-card"><KeyRound aria-hidden="true" size={20} /><p>Stored credentials are never returned to this console.</p></div>
        <div className="form-grid">
          <label>Replacement username<input autoComplete="username" value={credential.username} onChange={event => setCredential(value => ({ ...value, username: event.target.value }))} disabled={!editable || busy} required /></label>
          <label>Replacement password<input type="password" autoComplete="new-password" value={credential.password} onChange={event => setCredential(value => ({ ...value, password: event.target.value }))} minLength={12} disabled={!editable || busy} required /></label>
          <Button type="button" onClick={() => void replaceCredentials()} disabled={!editable || busy || !credential.username || credential.password.length < 12}>Replace credentials</Button>
        </div>
      </section>}
      {section !== 'sip' && <section className="settings-section">
        <h2>Device certificate</h2>
        <ValueRow label="Status" value="Not observed" />
        <Notice tone="info"><ShieldCheck aria-hidden="true" size={18} /> Certificate replacement requires the protected deployment workflow and is not exposed by this API.</Notice>
        <h2 className="section-gap">Accounts</h2>
        {accountRows.length === 0 ? <EmptyState title="No account details">No configured account metadata was returned.</EmptyState> : <div className="data-table" role="region" aria-label="Device accounts"><div className="data-table__head"><span>Account</span><span>Role</span><span>Action</span></div>{accountRows.map((entry, index) => {
          const username = readText(entry, 'username') ?? `Account ${index + 1}`
          return <div className="data-table__row" key={username}><span>{username}</span><span>{readText(entry, 'role') ?? 'Not reported'}</span><span>{pendingDelete === username ? <span className="inline-actions"><Button variant="danger" onClick={() => void deleteAccount(username)} disabled={busy}>Confirm</Button><Button onClick={() => setPendingDelete(null)} disabled={busy}>Cancel</Button></span> : <Button variant="danger" onClick={() => setPendingDelete(username)} disabled={!editable || busy}><Trash2 aria-hidden="true" size={16} /> Remove</Button>}</span></div>
        })}</div>}
        <h3 className="section-gap">Create or update account</h3>
        <div className="form-grid">
          <label>Username<input autoComplete="off" value={account.username} onChange={event => setAccount(value => ({ ...value, username: event.target.value.replace(/[^A-Za-z0-9_-]/g, '') }))} maxLength={32} disabled={!editable || busy} required /></label>
          <label>New password<input type="password" autoComplete="new-password" value={account.password} onChange={event => setAccount(value => ({ ...value, password: event.target.value }))} minLength={12} maxLength={256} disabled={!editable || busy} required /></label>
          <label>Role<select value={account.role} onChange={event => setAccount(value => ({ ...value, role: event.target.value as Role }))} disabled={!editable || busy}><option value="viewer">Viewer</option><option value="operator">Operator</option><option value="admin">Admin</option></select></label>
          <Button type="button" onClick={() => void saveAccount()} disabled={!editable || busy || !account.username || account.password.length < 12 || accountRevision < 1}><UserPlus aria-hidden="true" size={17} /> Save account</Button>
        </div>
        <ValueRow label="Account revision" value={accountRevision || 'Not reported'} />
        <ValueRow label="Settings revision" value={settingsRevision} />
      </section>}
      {!editable && <div className="settings-actions"><Notice tone="info">Only administrators can change accounts or credentials. Your role is {role}.</Notice></div>}
    </div>}
  </section>
}
