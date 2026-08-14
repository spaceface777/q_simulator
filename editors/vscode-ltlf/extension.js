const path = require('path');
const vscode = require('vscode');

const languageId = 'simulation-ltlf';
const defaultModuleIds = [...Array.from({ length: 24 }, (_, index) => index + 1), 255];

function activate(context) {
  const diagnostics = vscode.languages.createDiagnosticCollection('ltlf');
  const timers = new Map();
  const validator = loadValidator(context);

  const validate = async (document) => {
    if (document.languageId !== languageId) return;
    const diagnostic = await validateSource(await validator, document);
    if (!document.isClosed) diagnostics.set(document.uri, diagnostic ? [diagnostic] : []);
  };

  const schedule = (document) => {
    if (document.languageId !== languageId) return;
    const key = document.uri.toString();
    clearTimeout(timers.get(key));
    timers.set(key, setTimeout(() => {
      timers.delete(key);
      void validate(document);
    }, 200));
  };

  context.subscriptions.push(
    diagnostics,
    vscode.workspace.onDidOpenTextDocument(schedule),
    vscode.workspace.onDidChangeTextDocument((event) => schedule(event.document)),
    vscode.workspace.onDidCloseTextDocument((document) => {
      const key = document.uri.toString();
      clearTimeout(timers.get(key));
      timers.delete(key);
      diagnostics.delete(document.uri);
    }),
    vscode.workspace.onDidChangeConfiguration((event) => {
      if (event.affectsConfiguration('ltlf.moduleIds')) {
        for (const document of vscode.workspace.textDocuments) schedule(document);
      }
    }),
    { dispose: () => {
      for (const timer of timers.values()) clearTimeout(timer);
      timers.clear();
    } },
  );

  for (const document of vscode.workspace.textDocuments) schedule(document);
}

function loadValidator(context) {
  const createValidator = require(context.asAbsolutePath(path.join('wasm', 'validator.js')));
  return createValidator({
    locateFile: (filename) => context.asAbsolutePath(path.join('wasm', filename)),
  });
}

function writeString(module, value) {
  const size = module.lengthBytesUTF8(value) + 1;
  const pointer = module._malloc(size);
  module.stringToUTF8(value, pointer, size);
  return pointer;
}

async function validateSource(module, document) {
  const configured = vscode.workspace.getConfiguration('ltlf', document.uri).get('moduleIds', defaultModuleIds);
  const moduleIds = Array.isArray(configured)
    ? configured.filter((value) => Number.isInteger(value) && value >= 0 && value <= 255)
    : defaultModuleIds;
  const sourcePointer = writeString(module, document.getText());
  const modulesPointer = writeString(module, moduleIds.join(','));
  try {
    const resultPointer = module._ltlf_validate(sourcePointer, modulesPointer);
    if (!resultPointer) return undefined;
    const json = module.UTF8ToString(resultPointer);
    if (!json) return undefined;
    const result = JSON.parse(json);
    const line = Math.max(0, Math.min(result.line - 1, document.lineCount - 1));
    const text = document.lineAt(line).text;
    const column = Math.max(0, Math.min(result.column - 1, text.length));
    const end = Math.min(column + 1, text.length);
    const diagnostic = new vscode.Diagnostic(
      new vscode.Range(line, column, line, end),
      result.message,
      vscode.DiagnosticSeverity.Error,
    );
    diagnostic.source = 'LTLf';
    return diagnostic;
  } finally {
    module._free(modulesPointer);
    module._free(sourcePointer);
  }
}

function deactivate() {}

module.exports = { activate, deactivate };
