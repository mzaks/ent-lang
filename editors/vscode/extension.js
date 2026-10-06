// The ent-lang extension: starts the language server, ent-lsp, for the
// `.ent` files that are open. The colours are the grammar's
// (syntaxes/ent.tmLanguage.json) and need no server.
const fs = require('fs');
const path = require('path');
const vscode = require('vscode');
const { LanguageClient } = require('vscode-languageclient/node');

let client;

// The server: the one the settings name, else the one built in a folder
// that is open (build/bin/ent-lsp), else whatever the PATH has.
function serverPath() {
  const configured = vscode.workspace.getConfiguration('ent').get('server.path');
  if (configured) return configured;
  for (const folder of vscode.workspace.workspaceFolders || []) {
    const built = path.join(folder.uri.fsPath, 'build', 'bin', 'ent-lsp');
    if (fs.existsSync(built)) return built;
  }
  return 'ent-lsp';
}

async function start() {
  const configuration = vscode.workspace.getConfiguration('ent');
  client = new LanguageClient(
    'ent',
    'ent-lang',
    { command: serverPath(), args: configuration.get('server.arguments') || [] },
    { documentSelector: [{ scheme: 'file', language: 'ent' }] },
  );
  await client.start();
}

async function stop() {
  if (client) await client.stop();
  client = undefined;
}

exports.activate = async function (context) {
  context.subscriptions.push(
    vscode.commands.registerCommand('ent.restartServer', async () => {
      await stop();
      await start();
    }),
    vscode.workspace.onDidChangeConfiguration(async (change) => {
      if (change.affectsConfiguration('ent.server')) {
        await stop();
        await start();
      }
    }),
  );
  await start();
};

exports.deactivate = stop;
