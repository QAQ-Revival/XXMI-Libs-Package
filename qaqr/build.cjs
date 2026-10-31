const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawnSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');

function build({ output = path.join(root, 'build/component'), work = path.join(root, 'build/qaqr'), shader, verifyOnly = false } = {}) {
  output = path.resolve(output);
  work = path.resolve(work);
  const args = ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', path.join(__dirname, 'build.ps1'), '-WorkRoot', work, '-Test'];
  if (shader) args.push('-EfmiShader', path.resolve(shader));
  const result = spawnSync('powershell.exe', args, { cwd: root, stdio: 'inherit', windowsHide: true });
  if (result.error || result.status) throw result.error || Error('XXMI fork build or WARP validation failed');
  if (verifyOnly) return;
  const git = args => {
    const command = spawnSync('git', ['-c', 'user.name=qaq-revival-publisher[bot]', '-c', 'user.email=338412940+qaq-revival-publisher[bot]@users.noreply.github.com', '-c', 'credential.helper=', ...args], { cwd: root, windowsHide: true, encoding: 'utf8' });
    if (command.status) throw Error('Cannot identify or archive the runtime source');
    return command.stdout.trim();
  };
  if (git(['status', '--porcelain', '--ignore-submodules=all'])) throw Error('Commit the reviewed runtime source before packaging');
  fs.mkdirSync(output, { recursive: true });
  fs.copyFileSync(path.join(work, 'd3d11.dll'), path.join(output, 'd3d11.dll'));
  // Installed next to d3d11.dll, as docs/runtime-dependencies.md asks of a launcher.
  const libraries = JSON.parse(fs.readFileSync(path.join(root, 'Dependencies/runtime-dependencies.json'), 'utf8')).files.map(file => file.name);
  for (const name of libraries) fs.copyFileSync(path.join(root, 'Dependencies', name), path.join(output, name));
  for (const name of ['COPYING.txt', 'LICENSE.GPL.txt']) if (fs.existsSync(path.join(root, name))) fs.copyFileSync(path.join(root, name), path.join(output, name));
  git(['archive', '--format=zip', '--output=' + path.join(output, 'source.zip'), 'HEAD']);
  const files = {};
  for (const name of fs.readdirSync(output).filter(name => name !== 'component.json')) files[name] = crypto.createHash('sha256').update(fs.readFileSync(path.join(output, name))).digest('hex');
  fs.writeFileSync(path.join(output, 'component.json'), JSON.stringify({ source: 'https://github.com/QAQ-Revival/XXMI-Libs-Package', sourceCommit: git(['rev-parse', 'HEAD']), backend: 'vulkan', activityProtocol: 1, compatibleRuntimeHashes: require("./changes.json").compatibleRuntimeHashes, validation: 'D3D11 WARP', gameRuntimeValidated: false, files }, null, 2));
  return output;
}
if (require.main === module) build({ verifyOnly: process.argv.includes('--verify-only') });
module.exports = { build };
