# Cinatra - an efficient and easy-to-use C++ HTTP framework

## Table of Contents
* [Introduction](#introduction)
* [Usage](#usage)
* [Examples](#examples)
* Performance
* Caveats
* Roadmap
* Contact Information

## Introduction
Cinatra is a high-performance, easy-to-use http framework developed in Modern C++ (C++17) with the goal of making it easy and quick to develop web applications using the C++ programming language. Its main features are as follows:

1. Unified and simple interface,
2. Header-only,
3. Cross-platform,
4. Efficient
5. Support for AOP (aspect-oriented programming)

Cinatra currently supports HTTP 1.1/1.0, TLS/SSL and [WebSocket](https://www.wikiwand.com/en/WebSocket) protocols. You can use it to easily develop an HTTP server, such as a common database access server, a file upload/download server, real-time message push server, as well as a [MQTT](https://www.wikiwand.com/en/MQTT) server.

## Usage

Cinatra is a header-only library. So you can immediately use it in your code with a simple #include directive.

To compile your code with Cinatra, you need the following:

1. C++17 compiler (gcc 7.2, clang 4.0, Visual Studio 2017 update 15.5, or later versions)
2. Boost.Asio (or standalone Asio)
3. Boost.System
4. A UUID library (objbase.h for Windows, uuid.h for Linux, CFUUID.h for Mac)

## Examples


