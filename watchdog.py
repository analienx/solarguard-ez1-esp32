from host.bootstrap import ensure_runtime


if __name__ == "__main__":
    ensure_runtime()
    from host.watchdog import main

    raise SystemExit(main())