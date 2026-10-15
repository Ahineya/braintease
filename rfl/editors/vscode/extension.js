const vscode = require("vscode");
const { spawn } = require("child_process");
const fs = require("fs");
const path = require("path");

const TOKEN_TYPES = ["keyword", "function", "type", "parameter", "variable", "string", "number", "operator", "enumMember", "comment"];
const SELECTOR = { language: "rfl" };

function activate(context) {
  const connection = new Connection(serverCommand());
  context.subscriptions.push(connection);
  const legend = new vscode.SemanticTokensLegend(TOKEN_TYPES, []);
  context.subscriptions.push(
    vscode.languages.registerHoverProvider(SELECTOR, provider(connection, hover)),
    vscode.languages.registerCompletionItemProvider(SELECTOR, provider(connection, complete), ".", ":"),
    vscode.languages.registerDefinitionProvider(SELECTOR, provider(connection, definition)),
    vscode.languages.registerReferenceProvider(SELECTOR, provider(connection, references)),
    vscode.languages.registerRenameProvider(SELECTOR, provider(connection, rename)),
    vscode.languages.registerDocumentSymbolProvider(SELECTOR, provider(connection, symbols)),
    vscode.languages.registerSignatureHelpProvider(SELECTOR, provider(connection, signature), "(", ","),
    vscode.languages.registerInlayHintsProvider(SELECTOR, provider(connection, inlays)),
    vscode.languages.registerDocumentSemanticTokensProvider(SELECTOR, provider(connection, semanticTokens(legend)), legend)
  );
  for (const doc of vscode.workspace.textDocuments) {
    if (doc.languageId === "rfl") connection.open(doc);
  }
  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument((doc) => {
      if (doc.languageId === "rfl") connection.open(doc);
    }),
    vscode.workspace.onDidChangeTextDocument((event) => {
      if (event.document.languageId === "rfl") connection.change(event.document);
    }),
    vscode.workspace.onDidCloseTextDocument((doc) => {
      if (doc.languageId === "rfl") connection.close(doc);
    })
  );
}

function provider(connection, respond) {
  return {
    provideHover: (doc, pos) => respond(connection, doc, pos),
    provideCompletionItems: (doc, pos) => respond(connection, doc, pos),
    provideDefinition: (doc, pos) => respond(connection, doc, pos),
    provideReferences: (doc, pos) => respond(connection, doc, pos),
    provideRenameEdits: (doc, pos, name) => respond(connection, doc, pos, name),
    provideDocumentSymbols: (doc) => respond(connection, doc),
    provideSignatureHelp: (doc, pos) => respond(connection, doc, pos),
    provideInlayHints: (doc) => respond(connection, doc),
    provideDocumentSemanticTokens: (doc) => respond(connection, doc),
  };
}

function hover(connection, doc, pos) {
  return connection.request("textDocument/hover", textParams(doc, pos)).then((result) => {
    if (!result || !result.contents) return null;
    const text = typeof result.contents === "string" ? result.contents : result.contents.value;
    return new vscode.Hover(new vscode.MarkdownString(text));
  });
}

function complete(connection, doc, pos) {
  return connection.request("textDocument/completion", textParams(doc, pos)).then((items) => {
    return (items || []).map((item) => {
      const kind = typeof item.kind === "number" && item.kind > 0 ? item.kind - 1 : vscode.CompletionItemKind.Text;
      const converted = new vscode.CompletionItem(item.label, kind);
      converted.detail = item.detail || undefined;
      converted.filterText = item.filterText || item.label;
      if (item.textEdit && item.textEdit.range) {
        converted.range = toRange(item.textEdit.range);
        converted.insertText = item.textEdit.newText;
      }
      return converted;
    });
  });
}

function definition(connection, doc, pos) {
  return connection.request("textDocument/definition", textParams(doc, pos)).then((result) => (result ? toLocation(result) : null));
}

function references(connection, doc, pos) {
  return connection.request("textDocument/references", { ...textParams(doc, pos), context: { includeDeclaration: true } }).then((items) => {
    return (items || []).map(toLocation);
  });
}

function rename(connection, doc, pos, name) {
  return connection.request("textDocument/rename", { ...textParams(doc, pos), newName: name }).then((result) => {
    if (!result || !result.changes) return null;
    const edit = new vscode.WorkspaceEdit();
    for (const [uri, changes] of Object.entries(result.changes)) {
      for (const change of changes) {
        edit.replace(vscode.Uri.parse(uri), toRange(change.range), change.newText);
      }
    }
    return edit;
  });
}

function symbols(connection, doc) {
  return connection.request("textDocument/documentSymbol", { textDocument: { uri: doc.uri.toString() } }).then((items) => {
    return (items || []).map((item) => new vscode.DocumentSymbol(item.name, item.detail || "", item.kind, toRange(item.range), toRange(item.selectionRange)));
  });
}

function signature(connection, doc, pos) {
  return connection.request("textDocument/signatureHelp", textParams(doc, pos)).then((result) => {
    if (!result) return null;
    const help = new vscode.SignatureHelp();
    help.activeSignature = result.activeSignature || 0;
    help.activeParameter = result.activeParameter || 0;
    help.signatures = (result.signatures || []).map((sig) => {
      const item = new vscode.SignatureInformation(sig.label);
      item.parameters = (sig.parameters || []).map((param) => new vscode.ParameterInformation(param.label));
      return item;
    });
    return help;
  });
}

function inlays(connection, doc) {
  return connection.request("textDocument/inlayHint", { textDocument: { uri: doc.uri.toString() } }).then((items) => {
    return (items || []).map((hint) => {
      const item = new vscode.InlayHint(toPosition(hint.position), hint.label);
      item.paddingLeft = hint.paddingLeft === true;
      return item;
    });
  });
}

function semanticTokens() {
  return (connection, doc) => {
    return connection.request("textDocument/semanticTokens/full", { textDocument: { uri: doc.uri.toString() } }).then((result) => {
      return new vscode.SemanticTokens(new Uint32Array(result && result.data ? result.data : []));
    });
  };
}

function textParams(doc, pos) {
  return { textDocument: { uri: doc.uri.toString() }, position: { line: pos.line, character: pos.character } };
}

function toLocation(item) {
  return new vscode.Location(vscode.Uri.parse(item.uri), toRange(item.range));
}

function toRange(range) {
  return new vscode.Range(toPosition(range.start), toPosition(range.end));
}

function toPosition(position) {
  return new vscode.Position(position.line, position.character);
}

function serverCommand() {
  const configured = vscode.workspace.getConfiguration("rfl").get("serverPath");
  if (configured) return configured;
  for (const folder of vscode.workspace.workspaceFolders || []) {
    for (const relative of ["target/debug/rfl", "target/release/rfl"]) {
      const candidate = path.join(folder.uri.fsPath, relative);
      if (fs.existsSync(candidate)) return candidate;
    }
  }
  return "rfl";
}

class Connection {
  constructor(command) {
    this.pending = new Map();
    this.nextId = 1;
    this.buffer = Buffer.alloc(0);
    this.proc = spawn(command, ["lsp"], { stdio: ["pipe", "pipe", "pipe"] });
    this.proc.stdout.on("data", (chunk) => this.receive(chunk));
    this.proc.on("error", (err) => {
      vscode.window.showErrorMessage(`RFL language server failed to start (${command}): ${err.message}`);
    });
    this.proc.stderr.on("data", (chunk) => {
      const text = chunk.toString("utf8").trim();
      if (text) console.error(text);
    });
    this.request("initialize", {
      processId: process.pid,
      rootUri: (vscode.workspace.workspaceFolders || [])[0]?.uri.toString() || null,
      capabilities: {},
    }).then(() => this.notify("initialized", {}));
  }

  open(doc) {
    this.notify("textDocument/didOpen", { textDocument: { uri: doc.uri.toString(), languageId: "rfl", version: doc.version, text: doc.getText() } });
  }

  change(doc) {
    this.notify("textDocument/didChange", { textDocument: { uri: doc.uri.toString(), version: doc.version }, contentChanges: [{ text: doc.getText() }] });
  }

  close(doc) {
    this.notify("textDocument/didClose", { textDocument: { uri: doc.uri.toString() } });
  }

  request(method, params) {
    const id = this.nextId++;
    const promise = new Promise((resolve, reject) => this.pending.set(id, { resolve, reject }));
    this.send({ jsonrpc: "2.0", id, method, params });
    return promise;
  }

  notify(method, params) {
    this.send({ jsonrpc: "2.0", method, params });
  }

  send(message) {
    if (!this.proc.stdin.writable) return;
    const body = Buffer.from(JSON.stringify(message), "utf8");
    this.proc.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
    this.proc.stdin.write(body);
  }

  receive(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    for (;;) {
      const headerEnd = this.buffer.indexOf("\r\n\r\n");
      if (headerEnd < 0) return;
      const header = this.buffer.slice(0, headerEnd).toString("ascii");
      const match = /Content-Length:\s*(\d+)/i.exec(header);
      if (!match) {
        this.buffer = this.buffer.slice(headerEnd + 4);
        continue;
      }
      const length = Number(match[1]);
      const bodyStart = headerEnd + 4;
      if (this.buffer.length < bodyStart + length) return;
      const body = this.buffer.slice(bodyStart, bodyStart + length).toString("utf8");
      this.buffer = this.buffer.slice(bodyStart + length);
      this.dispatch(JSON.parse(body));
    }
  }

  dispatch(message) {
    if (message.method === "textDocument/publishDiagnostics") {
      const params = message.params || {};
      const uri = vscode.Uri.parse(params.uri);
      const diagnostics = (params.diagnostics || []).map((item) => {
        const diagnostic = new vscode.Diagnostic(toRange(item.range), item.message, item.severity === 1 ? vscode.DiagnosticSeverity.Error : vscode.DiagnosticSeverity.Warning);
        diagnostic.source = item.source || "rfl";
        return diagnostic;
      });
      this.diagnostics = this.diagnostics || vscode.languages.createDiagnosticCollection("rfl");
      this.diagnostics.set(uri, diagnostics);
      return;
    }
    if (message.id == null) return;
    const pending = this.pending.get(message.id);
    if (!pending) return;
    this.pending.delete(message.id);
    if (message.error) pending.reject(new Error(message.error.message || "language server error"));
    else pending.resolve(message.result);
  }

  dispose() {
    this.notify("shutdown", null);
    this.notify("exit", null);
    this.proc.kill();
    if (this.diagnostics) this.diagnostics.dispose();
  }
}

function deactivate() {}

module.exports = { activate, deactivate };
