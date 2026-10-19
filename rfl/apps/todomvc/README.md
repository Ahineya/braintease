# TodoMVC in RFL

```sh
python3 rcc-wasm/examples/browser/run.py --todo
```

Open the printed URL. This is an RFL/Wasm single-page application with hash
filters, local persistence, controlled editing and application-owned subscriptions.
No application model or persistence encoding lives in JavaScript.

- `model.rfl`: state, messages and typed filters.
- `codec.rfl`: bounded, versioned UTF-8 persistence records.
- `commands.rfl`: owned location/load/save/navigation work.
- `update.rfl`: pure state transitions and ordered save admission.
- `view.rfl`: typed HTML, stable keys, editing and keyboard/IME behavior.
- `main.rfl`: mount with initial commands.

See [routing and storage](../../../docs/rfl/browser-routing.md) for APIs, limits,
error handling, tests and the optional real-browser regression.
