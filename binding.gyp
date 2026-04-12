{
  "targets": [
    {
      "target_name": "nvideo",
      "sources": [
        "src/binding.cpp",
        "src/processor.cpp"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "src",
        "<(module_root_dir)/deps/ffmpeg/include"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS",
        "UNICODE",
        "_UNICODE"
      ],
      "cflags!": ["-fno-exceptions"],
      "cflags_cc!": ["-fno-exceptions"],
      "conditions": [
        ["OS=='win'", {
          "defines": [
            "WIN32_LEAN_AND_MEAN",
            "NOMINMAX",
            "__STDC_CONSTANT_MACROS"
          ],
          "msvs_settings": {
            "VCCLCompilerTool": {
              "ExceptionHandling": 1,
              "AdditionalOptions": ["/std:c++17"]
            },
            "VCLinkerTool": {
              "AdditionalLibraryDirectories": [
                "<(module_root_dir)/deps/win/lib"
              ]
            }
          },
          "msvs_toolset": "v143",
          "libraries": [
            "-lavformat",
            "-lavcodec",
            "-lavutil",
            "-lswscale",
            "-lswresample",
            "-lavfilter"
          ],
          "copies": [
            {
              "destination": "<(module_root_dir)/build/Release",
              "files": [
                "<(module_root_dir)/deps/win/bin/avformat-62.dll",
                "<(module_root_dir)/deps/win/bin/avcodec-62.dll",
                "<(module_root_dir)/deps/win/bin/avutil-60.dll",
                "<(module_root_dir)/deps/win/bin/swscale-9.dll",
                "<(module_root_dir)/deps/win/bin/swresample-6.dll",
                "<(module_root_dir)/deps/win/bin/avfilter-11.dll"
              ]
            }
          ]
        }],
        ["OS=='linux'", {
          "defines": [
            "__STDC_CONSTANT_MACROS"
          ],
          "cflags": ["-std=c++17"],
          "cflags_cc": ["-std=c++17"],
          "link_settings": {
            "library_dirs": [
              "<(module_root_dir)/deps/linux/lib"
            ],
            "libraries": [
              "-lavformat",
              "-lavcodec",
              "-lavutil",
              "-lswscale",
              "-lswresample",
              "-lavfilter"
            ]
          }
        }]
      ]
    }
  ]
}
