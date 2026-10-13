'use strict';

const vscode = require('vscode');
const { exec } = require('child_process');
const { promisify } = require('util');

const execAsync = promisify(exec);

/** @type {Array<any>} */
let portsCache = [];
let loading = false;
let timer = null;
let treeProvider = null;

const onDidChangeTreeData = new vscode.EventEmitter();
const statusBarItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);

function cfg() {
  return vscode.workspace.getConfiguration('nxport');
}

function nxportCommand() {
  return cfg().get('command') || 'nxport';
}

function quote(s) {
  return /\s/.test(s) ? `"${s}"` : s;
}

function buildUrl(p) {
  const tpl = cfg().get('openUrlTemplate') || 'http://localhost:{port}';
  return tpl.replace('{port}', String(p.local_port)).replace('{address}', String(p.local_address));
}

function isWebPort(p) {
  const n = Number(p.local_port);
  return [80, 443, 3000, 3001, 4000, 4173, 5000, 5001, 5173, 8000, 8001, 8080, 8081, 8082, 8083, 8443].includes(n);
}

async function fetchPorts() {
  if (loading) return portsCache;
  loading = true;
  try {
    const cmd = `${quote(nxportCommand())} --json`;
    const { stdout } = await execAsync(cmd, {
      maxBuffer: 32 * 1024 * 1024,
      windowsHide: true,
      timeout: 20000
    });
    const parsed = JSON.parse(String(stdout).trim() || '[]');
    const arr = Array.isArray(parsed) ? parsed : [];
    arr.sort((a, b) => Number(a.local_port) - Number(b.local_port));
    portsCache = arr;
    return portsCache;
  } catch (err) {
    portsCache = [];
    const msg = err && err.message ? err.message : String(err);
    if (/ENOENT|not recognized|not found|cannot find/i.test(msg)) {
      vscode.window.setStatusBarMessage(
        'NxPort not found — set "nxport.command" or add nxport to PATH',
        8000
      );
    }
    throw err;
  } finally {
    loading = false;
    onDidChangeTreeData.fire();
    updateStatusBar();
  }
}

function updateStatusBar() {
  statusBarItem.text = `$(list-flat) NxPort: ${portsCache.length}`;
  statusBarItem.tooltip = 'NxPort — show listening ports';
  statusBarItem.command = 'nxport.show';
  statusBarItem.show();
}

// ---------------------------------------------------------------------------
// Tree view
// ---------------------------------------------------------------------------
class PortTreeItem extends vscode.TreeItem {
  constructor(p) {
    super(String(p.local_port), vscode.TreeItemCollapsibleState.None);
    this.port = p;
    this.description = `${p.process} · ${p.local_address}`;
    this.tooltip = new vscode.MarkdownString(
      `**${p.process}** (pid \`${p.pid || '?'}\`)\n\n` +
        `- Address: \`${p.local_address}:${p.local_port}\`\n` +
        `- State: \`${p.state || 'Listen'}\`\n` +
        `- Dir: \`${p.dir || '-'}\``
    );
    this.iconPath = new vscode.ThemeIcon(
      isWebPort(p) ? 'globe' : 'radio-tower',
      new vscode.ThemeColor(isWebPort(p) ? 'charts.green' : 'charts.blue')
    );
    this.contextValue = 'nxport.port';
    this.command = { command: 'nxport.openPort', title: 'Open in Browser', arguments: [p] };
  }
}

class PortsTreeProvider {
  get onDidChangeTreeData() {
    return onDidChangeTreeData.event;
  }

  getTreeItem(item) {
    return item;
  }

  async getChildren() {
    if (portsCache.length === 0 && !loading) {
      try {
        await fetchPorts();
      } catch (_) {
        return [];
      }
    }
    return portsCache.map((p) => new PortTreeItem(p));
  }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
async function cmdRefresh(silent) {
  try {
    await fetchPorts();
    if (!silent) {
      vscode.window.setStatusBarMessage(`NxPort: ${portsCache.length} port(s) loaded`, 3000);
    }
  } catch (_) {
    /* reported in fetchPorts */
  }
}

async function pickPort() {
  if (portsCache.length === 0) await cmdRefresh(true);
  if (portsCache.length === 0) {
    vscode.window.showInformationMessage('NxPort: no listening ports found.');
    return undefined;
  }
  const items = portsCache.map((p) => ({
    label: `$(radio-tower) ${p.local_port}`,
    description: `${p.local_address} · ${p.process}`,
    detail: p.dir,
    port: p
  }));
  const picked = await vscode.window.showQuickPick(items, {
    placeHolder: 'Select a listening port',
    matchOnDescription: true,
    matchOnDetail: true
  });
  return picked ? picked.port : undefined;
}

async function openPort(p) {
  const port = p && p.local_port !== undefined ? p : await pickPort();
  if (!port) return;
  try {
    await vscode.env.openExternal(vscode.Uri.parse(buildUrl(port)));
  } catch (_) {
    vscode.window.showErrorMessage('NxPort: failed to open browser.');
  }
}

async function killPort(p) {
  const port = p && p.local_port !== undefined ? p : await pickPort();
  if (!port) return;
  const answer = await vscode.window.showWarningMessage(
    `Stop port ${port.local_port} (${port.process}, pid ${port.pid || '?'})?`,
    { modal: true },
    'Stop'
  );
  if (answer !== 'Stop') return;
  try {
    await execAsync(`${quote(nxportCommand())} kill ${port.local_port} -f`, {
      windowsHide: true,
      timeout: 20000
    });
    vscode.window.setStatusBarMessage(`NxPort: port ${port.local_port} stopped`, 4000);
    await cmdRefresh(true);
  } catch (_) {
    vscode.window.showErrorMessage(
      `NxPort: failed to stop port ${port.local_port}. ` +
        `On Windows, run VS Code as Administrator to stop system processes.`
    );
  }
}

async function copyUrl(p) {
  const port = p && p.local_port !== undefined ? p : await pickPort();
  if (!port) return;
  await vscode.env.clipboard.writeText(buildUrl(port));
}

async function copyDetails(p) {
  const port = p && p.local_port !== undefined ? p : await pickPort();
  if (!port) return;
  const text =
    `${port.local_address}:${port.local_port}  ${port.state || 'Listen'}  ` +
    `${port.process} (pid ${port.pid || '-'})  ${port.dir || ''}`;
  await vscode.env.clipboard.writeText(text);
}

async function showPorts() {
  await cmdRefresh(true);
  await openPort();
}

// ---------------------------------------------------------------------------
// Timer
// ---------------------------------------------------------------------------
function setupTimer() {
  if (timer) {
    clearInterval(timer);
    timer = null;
  }
  if (cfg().get('autoRefresh') === false) return;
  const interval = Math.max(1000, Number(cfg().get('refreshInterval')) || 5000);
  timer = setInterval(() => {
    if (!loading && vscode.window.state.focused) {
      fetchPorts().catch(() => {});
    }
  }, interval);
}

// ---------------------------------------------------------------------------
// Activate / Deactivate
// ---------------------------------------------------------------------------
function activate(context) {
  treeProvider = new PortsTreeProvider();

  context.subscriptions.push(
    vscode.window.createTreeView('nxport.ports', {
      treeDataProvider: treeProvider,
      showCollapseAll: false
    }),
    statusBarItem,
    onDidChangeTreeData,
    vscode.commands.registerCommand('nxport.show', showPorts),
    vscode.commands.registerCommand('nxport.refresh', () => cmdRefresh(false)),
    vscode.commands.registerCommand('nxport.openPort', openPort),
    vscode.commands.registerCommand('nxport.killPort', killPort),
    vscode.commands.registerCommand('nxport.copyUrl', copyUrl),
    vscode.commands.registerCommand('nxport.copyDetails', copyDetails),
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration('nxport')) {
        setupTimer();
        cmdRefresh(true);
      }
    }),
    { dispose: deactivate }
  );

  updateStatusBar();
  cmdRefresh(true);
  setupTimer();
}

function deactivate() {
  if (timer) {
    clearInterval(timer);
    timer = null;
  }
}

module.exports = { activate, deactivate };
