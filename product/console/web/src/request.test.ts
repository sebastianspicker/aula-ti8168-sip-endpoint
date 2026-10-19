import { afterEach, describe, expect, it, vi } from 'vitest'
import { requestJson } from './request'

function reply(status: number, body: unknown): Response {
  return new Response(typeof body === 'string' ? body : JSON.stringify(body), {
    status,
    headers: { 'content-type': 'application/json' }
  })
}

function stubFetch(...responses: Array<Response | Error>) {
  const fetch = vi.fn<typeof globalThis.fetch>()
  for (const response of responses) {
    if (response instanceof Error) fetch.mockRejectedValueOnce(response)
    else fetch.mockResolvedValueOnce(response)
  }
  vi.stubGlobal('fetch', fetch)
  return fetch
}

afterEach(() => {
  vi.unstubAllGlobals()
  vi.useRealTimers()
})

describe('requestJson', () => {
  it('returns data and revision from a valid envelope', async () => {
    stubFetch(reply(200, { revision: 1, ok: true, data: { call_state: 'idle' } }))
    await expect(requestJson('/zoom/api/v1/status', {}, false)).resolves.toEqual({
      data: { call_state: 'idle' },
      revision: 1
    })
  })

  it('surfaces the gateway error code and message', async () => {
    stubFetch(reply(403, { ok: false, error: { code: 'CSRF_INVALID', message: 'CSRF token is invalid' } }))
    await expect(requestJson('/x', {}, true)).rejects.toEqual({
      code: 'CSRF_INVALID',
      message: 'CSRF token is invalid',
      status: 403
    })
  })

  it('treats malformed envelopes and bodies as protocol errors', async () => {
    stubFetch(reply(200, { revision: 2, ok: true, data: {} }), reply(200, 'not json'), reply(200, [1, 2]))
    for (let index = 0; index < 3; index += 1)
      await expect(requestJson('/x', {}, false)).rejects.toMatchObject({ code: 'BACKEND_PROTOCOL', status: 502 })
  })

  it('retries a retryable request once after a network failure', async () => {
    const fetch = stubFetch(new TypeError('offline'), reply(200, { revision: 1, ok: true, data: 'ok' }))
    await expect(requestJson('/x', {}, true)).resolves.toMatchObject({ data: 'ok' })
    expect(fetch).toHaveBeenCalledTimes(2)
  })

  it('does not retry non-retryable requests or a second failure', async () => {
    let fetch = stubFetch(new TypeError('offline'))
    await expect(requestJson('/x', {}, false)).rejects.toMatchObject({ code: 'NETWORK_UNAVAILABLE' })
    expect(fetch).toHaveBeenCalledTimes(1)
    fetch = stubFetch(new TypeError('offline'), new TypeError('offline'))
    await expect(requestJson('/x', {}, true)).rejects.toMatchObject({ code: 'NETWORK_UNAVAILABLE' })
    expect(fetch).toHaveBeenCalledTimes(2)
  })

  it('retries BACKEND_UNAVAILABLE only for a 503', async () => {
    const unavailable = (status: number) =>
      reply(status, { ok: false, error: { code: 'BACKEND_UNAVAILABLE', message: 'down' } })
    let fetch = stubFetch(unavailable(503), reply(200, { revision: 1, ok: true, data: 1 }))
    await expect(requestJson('/x', {}, true)).resolves.toMatchObject({ data: 1 })
    expect(fetch).toHaveBeenCalledTimes(2)
    fetch = stubFetch(unavailable(502))
    await expect(requestJson('/x', {}, true)).rejects.toMatchObject({ code: 'BACKEND_UNAVAILABLE', status: 502 })
    expect(fetch).toHaveBeenCalledTimes(1)
  })

  it('rejects immediately when the caller already aborted', async () => {
    const fetch = stubFetch()
    const controller = new AbortController()
    controller.abort()
    await expect(requestJson('/x', {}, true, controller.signal)).rejects.toMatchObject({ code: 'REQUEST_ABORTED' })
    expect(fetch).not.toHaveBeenCalled()
  })

  it('times out a hanging request after ten seconds', async () => {
    vi.useFakeTimers()
    vi.stubGlobal('fetch', vi.fn(() => new Promise<Response>(() => {})))
    const pending = expect(requestJson('/x', {}, false)).rejects.toMatchObject({ code: 'REQUEST_TIMEOUT' })
    await vi.advanceTimersByTimeAsync(10_000)
    await pending
  })
})
