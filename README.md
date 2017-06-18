dTVProxy
========

This is a tool which causes movies from the online service dTV to be streamed over multiple connections when played inside Firefox. Due to the usual single connection behavior the service can be unwatchable at times, but using this tool movies can be enjoyed stutter-free in the highest quality on any connection.

# Overview

dTVProxy is a tiny application which consists of a three main parts:

## Firefox Interception

Upon startup of the application a piece of JavaScript code is sent to Firefox which causes requested movies from dTV to be streamed from localhost instead of directly. Due to the requests being proxied over our own server we can then split up the original request into multiple fast connections while still having the playback (and DRM decryption) handled by the browser. This real-time filter also removes all low quality stream info so the movies only play in the highest quality without the browser trying to switch between multiple quality levels.

## Local Webserver

A local webserver handles the proxied requests. Because files are buffered ahead of the clients request only the first part takes some time. All continued parts are instantly served from the buffer.

## Download Threads

A customizable number of threads (default 5) are downloading the streamed files in chunks (default 2 MB each) and automatically keep buffering ahead of the clients latest request (default is up to 64 MB ahead).

# Requirements

To intercept and modify the meta data request inside Firefox in real-time the extension [MozRepl](https://github.com/bard/mozrepl/wiki) needs to be installed and active.

# License

dTVProxy is available under the [Unlicense](http://unlicense.org/).
