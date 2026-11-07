import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { PollingLifecycle } from './pollingLifecycle'

describe('PollingLifecycle', () => {
  const doc = Object.assign(new EventTarget(), { hidden: false })
  beforeEach(() => { vi.stubGlobal('document', doc) })
  afterEach(() => { doc.hidden = false; vi.unstubAllGlobals() })

  it('drops the result of a read that was in flight when the page was hidden', async () => {
    let resolve: (value: string) => void = () => undefined
    const load = vi.fn(() => new Promise<string>(done => { resolve = done }))
    const success = vi.fn()
    const lifecycle = new PollingLifecycle({ load, success, failure: vi.fn() }, 1000)
    lifecycle.start()
    expect(load).toHaveBeenCalledTimes(1)
    doc.hidden = true
    doc.dispatchEvent(new Event('visibilitychange'))
    resolve('stale')
    await Promise.resolve()
    expect(success).not.toHaveBeenCalled()
    lifecycle.stop()
  })
})
