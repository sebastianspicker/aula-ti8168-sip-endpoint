import { RefreshCw } from 'lucide-react'
import { useCallback } from 'react'
import { api } from '../api'
import { useLivePolling } from '../hooks/useLivePolling'
import { Button, Notice } from '../components/ui'

const ignoreConnection = () => undefined

export function CollectionScreen({ page }: { page: 'library' | 'schedule' }) {
  const load = useCallback(async (signal: AbortSignal) => (await api[page](signal)).data, [page])
  const { error, refresh } = useLivePolling({ load, onConnection: ignoreConnection, intervalMs: 30000 })
  const title = page === 'library' ? 'Library' : 'Schedule'
  return <section aria-labelledby="collection-title">
    <div className="page-heading"><div><h1 id="collection-title">{title}</h1><p>{title} is unavailable in this console.</p></div><Button onClick={() => void refresh()}><RefreshCw aria-hidden="true" size={18} />Refresh</Button></div>
    <Notice tone="warning">{error?.code === 'CAPABILITY_UNAVAILABLE' ? 'This workflow is not yet implemented in the console.' : error ? 'Device information could not be loaded.' : 'Availability has not been verified.'}</Notice>
  </section>
}
