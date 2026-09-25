type PollingCallbacks<T> = {
  load: (signal: AbortSignal) => Promise<T>
  success: (data: T) => void
  failure: (error: unknown) => void
}

type PendingRead = { controller: AbortController; promise?: Promise<void> }

export class PollingLifecycle<T> {
  private stopped = false
  private hidden = document.hidden
  private pending = false
  private active: PendingRead | null = null
  private timer: ReturnType<typeof setTimeout> | undefined

  constructor(private callbacks: PollingCallbacks<T>, private intervalMs: number) {}

  start() {
    document.addEventListener('visibilitychange', this.visibilityChanged)
    void this.refresh()
  }

  stop() {
    this.stopped = true
    document.removeEventListener('visibilitychange', this.visibilityChanged)
    this.cancelRead()
  }

  refresh = (): Promise<void> => {
    if (this.stopped || this.hidden) return Promise.resolve()
    if (this.active) {
      this.pending = true
      return this.active.promise ?? Promise.resolve()
    }
    clearTimeout(this.timer)
    const read: PendingRead = { controller: new AbortController() }
    this.active = read
    read.promise = this.run(read)
    return read.promise
  }

  private async run(read: PendingRead) {
    try {
      const data = await this.callbacks.load(read.controller.signal)
      if (this.active === read) this.callbacks.success(data)
    } catch (error) {
      if (this.active === read && !read.controller.signal.aborted) this.callbacks.failure(error)
    } finally {
      if (this.active === read) {
        // A rejected aggregate loader may still have sibling requests running.
        read.controller.abort()
        this.finish()
      }
    }
  }

  private finish() {
    this.active = null
    if (this.pending) {
      this.pending = false
      void this.refresh()
    } else {
      this.timer = setTimeout(() => { void this.refresh() }, this.intervalMs)
    }
  }

  private cancelRead() {
    clearTimeout(this.timer)
    const read = this.active
    this.active = null
    this.pending = false
    read?.controller.abort()
  }

  private visibilityChanged = () => {
    if (document.hidden === this.hidden) return
    this.hidden = document.hidden
    if (this.hidden) this.cancelRead()
    else void this.refresh()
  }
}
