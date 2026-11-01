import { afterEach, describe, expect, it, vi } from 'vitest'
import { ConsoleApi } from './api'

function reply(status: number, body: unknown): Response {
  return new Response(JSON.stringify(body), { status, headers: { 'content-type': 'application/json' } })
}

function stubFetch(response: Response) {
  vi.stubGlobal('fetch', vi.fn<typeof globalThis.fetch>().mockResolvedValueOnce(response))
}

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('ConsoleApi session end', () => {
  it('reports an ended session when an authenticated read gets 401', async () => {
    stubFetch(reply(401, { revision: 1, ok: false, error: { code: 'SESSION_EXPIRED', message: 'session expired' } }))
    const api = new ConsoleApi()
    const ended = vi.fn()
    api.onSessionEnded = ended
    await expect(api.status()).rejects.toMatchObject({ code: 'SESSION_EXPIRED', status: 401 })
    expect(ended).toHaveBeenCalledTimes(1)
  })

  it('does not report a session end for a rejected login', async () => {
    stubFetch(reply(401, { revision: 1, ok: false, error: { code: 'AUTH_INVALID', message: 'invalid credentials' } }))
    const api = new ConsoleApi()
    const ended = vi.fn()
    api.onSessionEnded = ended
    await expect(api.login('admin', 'not-the-password')).rejects.toMatchObject({ code: 'AUTH_INVALID', status: 401 })
    expect(ended).not.toHaveBeenCalled()
  })
})
