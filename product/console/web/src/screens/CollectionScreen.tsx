import { RefreshCw } from 'lucide-react'
import { useCallback } from 'react'
import { api } from '../api'
import { useLivePolling } from '../hooks/useLivePolling'
import { Button, Notice } from '../components/ui'

const ignoreConnection = () => undefined

export function CollectionScreen({ page }: { page: 'library' | 'schedule' }) {
  const load = useCallback(async (signal: AbortSignal) => (await api[page](signal)).data, [page])
  const { error, refresh } = useLivePolling({ load, onConnection: ignoreConnection, intervalMs: 30000 })
  const library = page === 'library'
  return <section aria-labelledby="collection-title">
    <div className="page-heading"><div><h1 id="collection-title">{library ? 'Library' : 'Schedule'}</h1><p>{library ? 'Recordings and snapshots.' : 'Upcoming room operations.'}</p></div><Button onClick={() => void refresh()}><RefreshCw aria-hidden="true" size={18} />Refresh</Button></div>
    <Notice tone="warning">{error?.code === 'CAPABILITY_UNAVAILABLE' ? 'This workflow is not yet implemented in the console.' : error ? 'Device information could not be loaded.' : 'Device information has not been verified.'}</Notice>
    <p>{library ? 'Recordings could not be listed. Playback, downloads, metadata, deletion, and upload status are unavailable here.' : 'Device timezone and upcoming actions are unavailable. Conflicts and execution results cannot be shown.'}</p>
    {!library && <details><summary>Advanced · Scheduling integrations</summary><p>Local, Opencast, Panopto, Kaltura, and HTTP scheduling are not yet available in this console.</p></details>}
  </section>
}
