const fs = require('fs');
let code = fs.readFileSync('src/processor.cpp', 'utf8');

// Replace av_interleaved_write_frame to include unref
code = code.replace(/av_interleaved_write_frame\(([^,]+),\s*(pkt|packet)\);\s*(?!\s*av_packet_unref)/g, 'av_interleaved_write_frame(, );\n                    av_packet_unref();');

// Replace av_frame_get_buffer
code = code.replace(/av_frame_get_buffer\(([^,]+),\s*([^)]+)\);/g, 'if (av_frame_get_buffer(, ) < 0) { error.message = "Failed to allocate frame buffer"; error.operation = "allocate"; goto cleanup; }');

fs.writeFileSync('src/processor.cpp', code);
