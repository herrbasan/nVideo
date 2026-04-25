'use strict';

const https = require('https');
const fs = require('fs');
const path = require('path');
const { execSync } = require('child_process');

const REPO_ROOT = path.resolve(__dirname, '..');
const DEPS_DIR = path.join(REPO_ROOT, 'deps');
const FFMPEG_BASE_URL = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/';

const PLATFORM_MAP = {
    'win32-x64': {
        package: 'ffmpeg-master-latest-win64-gpl-shared.zip',
        extractDir: 'ffmpeg-master-latest-win64-gpl-shared',
        targetDir: 'win'
    },
    'linux-x64': {
        package: 'ffmpeg-master-latest-linux64-gpl-shared.tar.xz',
        extractDir: 'ffmpeg-master-latest-linux64-gpl-shared',
        targetDir: 'linux'
    }
};

function getPlatformKey() {
    const platform = process.platform;
    const arch = process.arch;
    if (platform === 'win32' && arch === 'x64') return 'win32-x64';
    if (platform === 'linux' && arch === 'x64') return 'linux-x64';
    throw new Error(`Unsupported platform: ${platform}-${arch}`);
}

function downloadFile(url, destPath) {
    return new Promise((resolve, reject) => {
        console.log(`Downloading: ${url}`);
        const file = fs.createWriteStream(destPath);

        function handleResponse(response) {
            if (response.statusCode === 302 || response.statusCode === 301) {
                https.get(response.headers.location, handleResponse).on('error', reject);
                return;
            }
            const totalBytes = parseInt(response.headers['content-length'], 10);
            let downloadedBytes = 0;
            response.on('data', (chunk) => {
                downloadedBytes += chunk.length;
                const percent = ((downloadedBytes / totalBytes) * 100).toFixed(1);
                process.stdout.write(`\rProgress: ${percent}% (${(downloadedBytes / 1024 / 1024).toFixed(1)} MB / ${(totalBytes / 1024 / 1024).toFixed(1)} MB)`);
            });
            response.pipe(file);
            file.on('finish', () => {
                file.close();
                console.log('\nDownload complete');
                resolve();
            });
        }

        https.get(url, handleResponse).on('error', (err) => {
            fs.unlink(destPath, () => {});
            reject(err);
        });
    });
}

function extractArchive(archivePath, extractDir) {
    console.log(`Extracting: ${path.basename(archivePath)}`);
    if (archivePath.endsWith('.zip')) {
        execSync(`powershell -Command "Expand-Archive -Path '${archivePath}' -DestinationPath '${extractDir}' -Force"`, { stdio: 'inherit' });
    } else if (archivePath.endsWith('.tar.xz')) {
        execSync(`tar -xJf "${archivePath}" -C "${extractDir}"`, { stdio: 'inherit' });
    }
    console.log('Extraction complete');
}

function copyDirRecursive(src, dest) {
    fs.mkdirSync(dest, { recursive: true });
    for (const entry of fs.readdirSync(src, { withFileTypes: true })) {
        const srcPath = path.join(src, entry.name);
        const destPath = path.join(dest, entry.name);
        if (entry.isDirectory()) {
            copyDirRecursive(srcPath, destPath);
        } else {
            fs.copyFileSync(srcPath, destPath);
        }
    }
}

function organizeFiles(extractedPath) {
    const platform = getPlatformKey();
    const platformTargetDir = path.join(DEPS_DIR, PLATFORM_MAP[platform].targetDir);
    const binDir = path.join(platformTargetDir, 'bin');
    const libDir = path.join(platformTargetDir, 'lib');
    const includeDir = path.join(DEPS_DIR, 'ffmpeg', 'include');

    fs.mkdirSync(binDir, { recursive: true });
    fs.mkdirSync(libDir, { recursive: true });
    fs.mkdirSync(includeDir, { recursive: true });

    const srcBinDir = path.join(extractedPath, 'bin');
    if (fs.existsSync(srcBinDir)) {
        const binaries = fs.readdirSync(srcBinDir).filter(f => f.endsWith('.dll') || f.endsWith('.so') || f.endsWith('.exe') || (!f.includes('.')));
        binaries.forEach(file => fs.copyFileSync(path.join(srcBinDir, file), path.join(binDir, file)));
        console.log(`Copied ${binaries.length} binary files`);
    }

    const srcLibDir = path.join(extractedPath, 'lib');
    if (fs.existsSync(srcLibDir)) {
        const libs = fs.readdirSync(srcLibDir).filter(f => f.endsWith('.lib') || f.endsWith('.so') || f.endsWith('.a'));
        libs.forEach(file => fs.copyFileSync(path.join(srcLibDir, file), path.join(libDir, file)));
        console.log(`Copied ${libs.length} library files`);
    }

    const srcIncludeDir = path.join(extractedPath, 'include');
    if (fs.existsSync(srcIncludeDir)) {
        copyDirRecursive(srcIncludeDir, includeDir);
        console.log('Copied header files');
    }
}

async function main() {
    console.log('FFmpeg Download & Setup');
    console.log('='.repeat(60));

    const platformKey = getPlatformKey();
    const platformInfo = PLATFORM_MAP[platformKey];
    console.log(`Platform: ${platformKey}`);
    console.log(`Package: ${platformInfo.package}\n`);

    fs.mkdirSync(DEPS_DIR, { recursive: true });

    const targetDir = path.join(DEPS_DIR, platformInfo.targetDir);
    const headersExist = fs.existsSync(path.join(DEPS_DIR, 'ffmpeg', 'include', 'libavformat'));
    const binariesExist = fs.existsSync(path.join(targetDir, 'bin'));

    if (headersExist && binariesExist) {
        console.log('FFmpeg libraries already present, skipping download');
        return;
    }

    const downloadUrl = FFMPEG_BASE_URL + platformInfo.package;
    const archivePath = path.join(DEPS_DIR, platformInfo.package);

    if (!fs.existsSync(archivePath)) {
        await downloadFile(downloadUrl, archivePath);
    } else {
        console.log('Archive already downloaded, skipping download');
    }

    const tempExtractDir = path.join(DEPS_DIR, 'temp');
    fs.mkdirSync(tempExtractDir, { recursive: true });
    extractArchive(archivePath, tempExtractDir);

    const extractedPath = path.join(tempExtractDir, platformInfo.extractDir);
    organizeFiles(extractedPath);

    console.log('Cleaning up temporary files...');
    fs.rmSync(tempExtractDir, { recursive: true, force: true });
    fs.unlinkSync(archivePath);

    console.log('\n' + '='.repeat(60));
    console.log('FFmpeg setup complete');
    console.log(`   Headers:   ${path.join(DEPS_DIR, 'ffmpeg', 'include')}`);
    console.log(`   Binaries:  ${path.join(targetDir, 'bin')}`);
    console.log(`   Libraries: ${path.join(targetDir, 'lib')}`);
}

if (require.main === module) {
    main().catch(err => {
        console.error('Error:', err.message);
        process.exit(1);
    });
}

module.exports = { main };
