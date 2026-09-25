import { LockKeyhole, ShieldCheck } from 'lucide-react'
import { useState } from 'react'
import { api } from '../api'
import type { ApiError, Role } from '../types'
import { Button, ErrorNotice, Notice } from './ui'

export function Auth({ onAuthenticated }: { onAuthenticated: (role: Role) => void }) {
  const [mode, setMode] = useState<'login' | 'bootstrap'>('login')
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [bootstrapCode, setBootstrapCode] = useState('')
  const [error, setError] = useState<ApiError | null>(null)
  const [busy, setBusy] = useState(false)

  const submit = async (event: React.FormEvent) => {
    event.preventDefault(); setError(null); setBusy(true)
    try {
      if (mode === 'bootstrap') {
        await api.bootstrap(username, password, bootstrapCode)
        const session = await api.login(username, password)
        onAuthenticated(session.data.role)
      } else {
        const session = await api.login(username, password)
        onAuthenticated(session.data.role)
      }
    } catch (cause) { setError(cause as ApiError) } finally { setBusy(false) }
  }

  return <main className="auth-page"><section className="auth-card" aria-labelledby="auth-title"><div className="auth-mark"><ShieldCheck aria-hidden="true" size={28} /></div><h1 id="auth-title">LS200 Console</h1><p>{mode === 'login' ? 'Sign in to view live device state and permitted controls.' : 'Create the one-time administrator account.'}</p><ErrorNotice error={error} />
    <form onSubmit={submit} className="form-stack">
      <label>Username<input autoComplete="username" value={username} onChange={event => setUsername(event.target.value)} minLength={1} maxLength={32} pattern={mode === 'bootstrap' ? '[A-Za-z0-9_\\-]+' : undefined} required /></label>
      <label>Password<input type="password" autoComplete={mode === 'login' ? 'current-password' : 'new-password'} value={password} onChange={event => setPassword(event.target.value)} minLength={mode === 'bootstrap' ? 12 : 1} maxLength={256} required /></label>
      {mode === 'bootstrap' && <label>One-time bootstrap code<input type="password" autoComplete="one-time-code" value={bootstrapCode} onChange={event => setBootstrapCode(event.target.value)} minLength={16} maxLength={128} required /><small>This value is sent only to establish the first administrator account.</small></label>}
      <Button variant="primary" type="submit" disabled={busy}>{busy ? 'Working…' : mode === 'login' ? 'Sign in' : 'Create administrator account'}</Button>
    </form>
    <Notice tone="info"><LockKeyhole aria-hidden="true" size={17} /><span>Passwords are never displayed after entry.</span></Notice>
    <button className="text-action" onClick={() => { setMode(mode === 'login' ? 'bootstrap' : 'login'); setError(null) }}>{mode === 'login' ? 'Set up this device for the first time' : 'Return to sign in'}</button>
  </section></main>
}
