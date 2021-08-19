{
  "targets": [
    {
      # myModule is the name of your native addon
      "target_name": "hello",

      'include_dirs': ["<!(node -p \"require('node-addon-api').include_dir\")"],
      "sources": [
        "src/hello.cc",
      ],
      'defines': [ 'NAPI_CPP_EXCEPTIONS' ],
      'cflags!': ['-ansi'],
      'conditions': [
          ['OS == "win"',
              {'defines': ['CORO_FIBER', 'WINDOWS']},
          # else
              {
                  'defines': ['USE_CORO', 'CORO_GUARDPAGES=1'],
                  'ldflags': ['-pthread'],
              }
          ],
          ['OS == "linux"',
              {
                  'cflags_c': [ '-std=gnu11' ],
                  'variables': {
                      'USE_MUSL': '<!(ldd --version 2>&1 | head -n1 | grep "musl" | wc -l)',
                  },
                  'conditions': [
                      ['<(USE_MUSL) == 1',
                          {'defines': ['CORO_ASM', '__MUSL__']},
                          {'defines': ['CORO_UCONTEXT']}
                      ],
                  ],
              },
          ],
          ['OS == "solaris" or OS == "sunos" or OS == "freebsd" or OS == "aix"', {'defines': ['CORO_UCONTEXT']}],
          ['OS == "mac"', {'defines': ['CORO_ASM']}],
          ['OS == "openbsd"', {'defines': ['CORO_ASM']}],
          ['target_arch == "arm" or target_arch == "arm64"',
              {
                  # There's been problems getting real fibers working on arm
                  'defines': ['CORO_PTHREAD'],
                  'defines!': ['CORO_UCONTEXT', 'CORO_SJLJ', 'CORO_ASM'],
              },
          ],
      ],
    }
  ]
}
