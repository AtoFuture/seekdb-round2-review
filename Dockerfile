# Native official JSONL runtime; dev-tsv remains an explicit development option.
#
# Build robustness notes (2026-11-12):
# * The base image stays pinned by digest, so libc and the runtime ABI are fixed.
# * Alpine moves superseded patch releases out of the mirror. An exact
#   `pkg=x.y-rN` pin therefore becomes a hard build failure once upstream bumps
#   that package, which would cost the whole submission (no image -> no
#   admission). The exact pin is still attempted first; the fallback only
#   relaxes the patch level with apk's fuzzy match and keeps the minor locked.
# * `apk add --no-cache` fetches live indexes. A truncated index download shows up
#   as "no such package" for one package only, so the fetch is retried.
# * Only the `minivectordb` target is built in the build stage. Building every
#   test executable multiplies platform build time and failure surface; the test
#   stage builds them on demand.
FROM alpine:3.22.8@sha256:49340b346798875b5592e5d420241f2f9cfba5f47e62ebb1446a1b34734e898f AS build
RUN set -eux; \
    installed=0; \
    for attempt in 1 2 3; do \
      if apk add --no-cache build-base=0.5-r3 cmake=3.31.7-r1 samurai=1.2-r7 python3=3.12.16-r0; then \
        installed=1; break; \
      fi; \
      sleep 5; \
    done; \
    if [ "$installed" != "1" ]; then \
      apk add --no-cache 'build-base~0.5' 'cmake~3.31' 'samurai~1.2' 'python3~3.12'; \
    fi
WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tests ./tests
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --target minivectordb --parallel 2

FROM build AS test
ENV MVDB_TEST_DIR=/data/core-tests
ENV PYTHONDONTWRITEBYTECODE=1
WORKDIR /build
RUN cmake --build /build --parallel 2
CMD ["ctest", "--test-dir", "/build", "--output-on-failure"]

FROM alpine:3.22.8@sha256:49340b346798875b5592e5d420241f2f9cfba5f47e62ebb1446a1b34734e898f AS runtime
RUN set -eux; \
    installed=0; \
    for attempt in 1 2 3; do \
      if apk add --no-cache libstdc++=14.2.0-r6; then \
        installed=1; break; \
      fi; \
      sleep 5; \
    done; \
    if [ "$installed" != "1" ]; then \
      apk add --no-cache 'libstdc++~14.2'; \
    fi
COPY --from=build /build/minivectordb /usr/local/bin/minivectordb
WORKDIR /data
ENTRYPOINT ["/usr/local/bin/minivectordb", "--protocol", "jsonl", "--data-dir", "/data"]
