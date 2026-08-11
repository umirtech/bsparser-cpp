const childProcess = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const [referencePath, bsparsePath, fixtureDirectory] = process.argv.slice(2);

if (!referencePath || !bsparsePath || !fixtureDirectory) {
  throw new Error('usage: compare-reference.js <parser.js> <bsparse> <test-files>');
}

const referenceSource = fs.readFileSync(referencePath, 'utf8');

function referenceKind(header) {
  if (header['@type'] === 'IVF') return 'ivf';
  if ('nal_unit_type' in header) return `nal:${header.nal_unit_type}`;
  if ('obu_type' in header) return `obu:${header.obu_type}`;
  return 'frame';
}

function cppKind(header) {
  if (header.type === 'IVF') return 'ivf';
  if ('nal_unit_type' in header.fields) return `nal:${header.fields.nal_unit_type}`;
  if ('obu_type' in header.fields) return `obu:${header.fields.obu_type}`;
  return 'frame';
}

function parseWithReference(format, fixturePath) {
  const context = {Uint8Array};
  vm.createContext(context);
  vm.runInContext(referenceSource, context, {filename: referencePath});

  const parser = context.create_parser(format);
  if (parser === null) throw new Error(`reference parser does not support ${format}`);

  const bytes = fs.readFileSync(fixturePath);
  const chunkSize = 4093;
  for (let offset = 0; offset < bytes.length; offset += chunkSize) {
    parser.parse(new Uint8Array(bytes.subarray(offset, Math.min(offset + chunkSize, bytes.length))));
  }
  parser.parse(null);

  return context.g_headers.map((header) => ({
    kind: referenceKind(header),
  }));
}

function parseWithCpp(format, fixturePath) {
  const result = childProcess.spawnSync(bsparsePath, [format, fixturePath], {encoding: 'utf8'});
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(result.stderr);

  return result.stdout
      .trim()
      .split('\n')
      .filter(Boolean)
      .map((line) => {
        const header = JSON.parse(line);
        return {kind: cppKind(header)};
      });
}

function compareFixture(format, name) {
  const fixturePath = path.join(fixtureDirectory, name);
  const referenceHeaders = parseWithReference(format, fixturePath);
  const cppHeaders = parseWithCpp(format, fixturePath);

  if (referenceHeaders.length !== cppHeaders.length) {
    throw new Error(`${name}: header count differs (reference=${referenceHeaders.length}, cpp=${cppHeaders.length})`);
  }

  for (let index = 0; index < referenceHeaders.length; ++index) {
    const referenceHeader = referenceHeaders[index];
    const cppHeader = cppHeaders[index];
    for (const key of ['kind']) {
      if (referenceHeader[key] === cppHeader[key]) continue;
      throw new Error(
          `${name}: header ${index} differs for ${key} ` +
          `(reference=${referenceHeader[key]}, cpp=${cppHeader[key]})`);
    }
  }
}

compareFixture('h264', 'h264.h264');
compareFixture('h265', 'h265.h265');
compareFixture('ivf', 'vp8.ivf');
compareFixture('ivf', 'vp9.ivf');
