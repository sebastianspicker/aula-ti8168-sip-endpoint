import { BookUser, RefreshCw, Trash2 } from 'lucide-react'
import { useCallback, useState } from 'react'
import { api } from '../api'
import type { ApiError, DirectoryRequest, JsonRecord, Role } from '../types'
import { isRecord, readNumber, readText } from '../types'
import { Button, EmptyState, ErrorNotice, LoadingRows, Notice } from '../components/ui'
import { useLivePolling } from '../hooks/useLivePolling'

const EMPTY_ENTRY: DirectoryRequest = {
  id: '', name: '', meeting_id: '', profile: 'zoom_direct', default_layout: 'full_screen'
}

export function DirectoryScreen({ role, onConnection }: {
  role: Role
  onConnection: (state: 'live' | 'stale' | 'offline') => void
}) {
  const [form, setForm] = useState<DirectoryRequest>(EMPTY_ENTRY)
  const [actionError, setActionError] = useState<ApiError | null>(null)
  const [message, setMessage] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)
  const [confirmDelete, setConfirmDelete] = useState<string | null>(null)
  const loadDirectory = useCallback(async (signal: AbortSignal): Promise<JsonRecord> => (await api.directory(signal)).data, [])
  const { data: directory, error, refresh } = useLivePolling({ load: loadDirectory, onConnection })
  const entries = directory && Array.isArray(directory.entries) ? directory.entries.filter(isRecord) : []
  const revision = readNumber(directory, 'revision') ?? 0
  const canManage = role === 'admin'
  const update = <K extends keyof DirectoryRequest>(key: K, value: DirectoryRequest[K]) =>
    setForm(previous => ({ ...previous, [key]: value }))
  const perform = async (operation: () => Promise<unknown>, success: string) => {
    setBusy(true); setActionError(null); setMessage(null)
    try {
      await operation(); setMessage(success); await refresh(); return true
    } catch (cause) {
      setActionError(cause as ApiError); return false
    } finally { setBusy(false) }
  }
  const save = async () => {
    if (await perform(() => api.saveDirectory(form, revision), 'Directory entry saved.')) setForm(EMPTY_ENTRY)
  }
  const remove = async (id: string) => {
    if (await perform(() => api.deleteDirectory(id, revision), 'Directory entry removed.')) setConfirmDelete(null)
  }

  return <section aria-labelledby="directory-title">
    <div className="page-heading"><div><h1 id="directory-title">Directory</h1><p>Saved meetings contain only non-sensitive dialing references.</p></div><Button onClick={() => void refresh()}><RefreshCw aria-hidden="true" size={17} /> Refresh</Button></div>
    <ErrorNotice error={actionError ?? error} />{message && <Notice tone="success">{message}</Notice>}
    {!directory ? <LoadingRows /> : <>
      {entries.length === 0 ? <EmptyState title="Directory is empty">No safe directory entries are configured.</EmptyState> :
        <div className="data-table directory-table" role="region" aria-label="Device directory" tabIndex={0}>
          <div className="data-table__head"><span>Name</span><span>Meeting ID</span><span>Policy</span><span>Action</span></div>
          {entries.map((entry, index) => {
            const id = readText(entry, 'id') ?? `entry-${index}`
            return <div className="data-table__row" key={id}>
              <span><BookUser aria-hidden="true" size={18} />{readText(entry, 'name') ?? 'Unlabelled entry'}</span>
              <span>{readText(entry, 'meeting_id') ?? 'Not reported'}</span>
              <span>{[readText(entry, 'profile'), readText(entry, 'default_layout')].filter(Boolean).join(' · ') || 'Not reported'}</span>
              <span>{confirmDelete === id ? <span className="inline-actions"><Button variant="danger" onClick={() => void remove(id)} disabled={busy}>Confirm</Button><Button onClick={() => setConfirmDelete(null)} disabled={busy}>Cancel</Button></span> : <Button variant="danger" onClick={() => setConfirmDelete(id)} disabled={!canManage || busy}><Trash2 aria-hidden="true" size={16} /> Remove</Button>}</span>
            </div>
          })}
        </div>}
      {canManage ? <section className="settings-section section-gap" aria-labelledby="directory-editor-title">
        <h2 id="directory-editor-title">Create or update entry</h2>
        <div className="form-grid">
          <label>Reference ID<input value={form.id} onChange={event => update('id', event.target.value.replace(/[^A-Za-z0-9_-]/g, ''))} maxLength={32} disabled={busy} required /></label>
          <label>Name<input value={form.name} onChange={event => update('name', event.target.value.replace(/[\\/@%]/g, ''))} maxLength={64} disabled={busy} required /></label>
          <label>Meeting ID<input inputMode="numeric" pattern="[0-9]{9,11}" value={form.meeting_id} onChange={event => update('meeting_id', event.target.value.replace(/\D/g, ''))} minLength={9} maxLength={11} disabled={busy} required /></label>
          <label>Profile<select value={form.profile} onChange={event => update('profile', event.target.value as DirectoryRequest['profile'])} disabled={busy}><option value="zoom_direct">Zoom direct</option><option value="zoom_proxy">Zoom proxy</option><option value="private_lab">Private lab</option></select></label>
          <label>Default layout<select value={form.default_layout} onChange={event => update('default_layout', event.target.value as DirectoryRequest['default_layout'])} disabled={busy}><option value="full_screen">Full screen</option><option value="gallery">Gallery</option><option value="dual_video">Dual video</option></select></label>
          <Button variant="primary" type="button" onClick={() => void save()} disabled={busy || revision < 1 || !form.id || !form.name || form.meeting_id.length < 9}>Save entry</Button>
        </div>
      </section> : <Notice tone="info">Only administrators can change directory entries.</Notice>}
    </>}
    <Notice tone="info">Passcodes, host keys, participant codes, credentials, and SIP destinations are never stored in the directory.</Notice>
  </section>
}
