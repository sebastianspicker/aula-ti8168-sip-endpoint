import type { ButtonHTMLAttributes, ReactNode } from 'react'
import { AlertCircle, CheckCircle2, Circle, LoaderCircle, WifiOff } from 'lucide-react'
import type { ApiError } from '../types'

export function Button({ variant = 'secondary', className = '', ...props }: ButtonHTMLAttributes<HTMLButtonElement> & { variant?: 'primary' | 'secondary' | 'quiet' | 'danger' }) {
  return <button className={`button button--${variant} ${className}`} {...props} />
}

export function Notice({ children, tone = 'info' }: { children: ReactNode; tone?: 'info' | 'error' | 'warning' | 'success' }) {
  const Icon = tone === 'error' ? AlertCircle : tone === 'success' ? CheckCircle2 : Circle
  return <div className={`notice notice--${tone}`} role={tone === 'error' ? 'alert' : 'status'}><Icon aria-hidden="true" size={18} />{children}</div>
}

export function ErrorNotice({ error }: { error: ApiError | null }) {
  if (!error) return null
  return <Notice tone="error"><span><strong>{error.code.replaceAll('_', ' ')}</strong> {error.message}</span></Notice>
}

export function LoadingRows({ count = 3 }: { count?: number }) {
  return <div aria-label="Loading device data" aria-live="polite" role="status" className="loading-rows">{Array.from({ length: count }, (_, index) => <div aria-hidden="true" className="skeleton" key={index} />)}</div>
}

export function EmptyState({ title, children }: { title: string; children: ReactNode }) {
  return <div className="empty-state"><h3>{title}</h3><p>{children}</p></div>
}

export function Freshness({ state, label }: { state: 'live' | 'stale' | 'offline' | 'loading'; label: string }) {
  const Icon = state === 'offline' ? WifiOff : state === 'loading' ? LoaderCircle : state === 'live' ? CheckCircle2 : AlertCircle
  return <span className={`freshness freshness--${state}`}><Icon aria-hidden="true" size={16} className={state === 'loading' ? 'spin' : ''} />{label}</span>
}

export function ValueRow({ label, value, tone }: { label: string; value: ReactNode; tone?: 'success' | 'warning' | 'danger' }) {
  return <div className="value-row"><span>{label}</span><strong className={tone ? `text-${tone}` : ''}>{value}</strong></div>
}
