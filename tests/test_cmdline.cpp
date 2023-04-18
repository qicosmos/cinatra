#include <string>

#include "cmdline.h"
#include "doctest.h"

// ./simple_test -i input.txt --output=output.txt -vt100
TEST_CASE("simple test cmd line options") {
  const char* argv[] = {
      "simple_test", "-i", "input.txt", "--output=output.txt", "-t4",
  };
  int argc = sizeof(argv) / sizeof(argv[0]);
  cmdline::parser p;

  p.add<std::string>("input", 'i', "input file");
  p.add<std::string>("output", 'o', "output file");
  p.add<int>("threads", 't', "total number of threads to use", false, 1);

  p.parse_check(argc, const_cast<char**>(argv));

  CHECK(p.get<std::string>("input") == "input.txt");
  CHECK(p.get<std::string>("output") == "output.txt");
  CHECK(p.get<int>("threads") == 4);
}

// ./cinatra_press_tool -c100 -t4 -d10s --headers=HTTPheaders -r7
TEST_CASE("test cinatra_press_tool cmd line options") {
  const char* argv[] = {"cinatra_press_tool",    "-c100", "-vt4", "-d10s",
                        "--headers=HTTPheaders", "-r7"};
  int argc = sizeof(argv) / sizeof(argv[0]);
  cmdline::parser p;

  p.add<int>("connections", 'c',
             "total number of HTTP connections to keep open with"
             "                   each thread handling N = connections/threads");
  p.add<std::string>("duration", 'd', "duration of the test, e.g. 2s, 2m, 2h",
                     false, "15s");
  p.add<int>("threads", 't', "total number of threads to use", false, 1);
  p.add<std::string>(
      "headers", 'H',
      "HTTP headers to add to request, e.g. \"User-Agent: coro_http_press\"\n"
      "            add multiple http headers in a request need to be separated "
      "by ' && '\n"
      "            e.g. \"User-Agent: coro_http_press && x-frame-options: "
      "SAMEORIGIN\"",
      false, "");
  p.add<int>("readfix", 'r', "read fixed response", false, 0);
  p.add("version", 'v', "Display version information");

  p.parse_check(argc, const_cast<char**>(argv));

  CHECK(p.get<int>("connections") == 100);
  CHECK(p.get<int>("threads") == 4);
  CHECK(p.get<std::string>("duration") == "10s");
  CHECK(p.get<std::string>("headers") == "HTTPheaders");
  CHECK(p.get<int>("readfix") == 7);
  CHECK(p.exist("version"));
}

// ./cinatra_press_tool -c 100 -t 4 -d 10s --headers=HTTPheaders -r 7
TEST_CASE("test cinatra_press_tool cmd line options") {
  const char* argv[] = {
      "cinatra_press_tool",    "-c", "100", "-v", "-t", "4", "-d", "10s",
      "--headers=HTTPheaders", "-r", "7"};
  int argc = sizeof(argv) / sizeof(argv[0]);
  cmdline::parser p;

  p.add<int>("connections", 'c',
             "total number of HTTP connections to keep open with"
             "                   each thread handling N = connections/threads");
  p.add<std::string>("duration", 'd', "duration of the test, e.g. 2s, 2m, 2h",
                     false, "15s");
  p.add<int>("threads", 't', "total number of threads to use", false, 1);
  p.add<std::string>(
      "headers", 'H',
      "HTTP headers to add to request, e.g. \"User-Agent: coro_http_press\"\n"
      "            add multiple http headers in a request need to be separated "
      "by ' && '\n"
      "            e.g. \"User-Agent: coro_http_press && x-frame-options: "
      "SAMEORIGIN\"",
      false, "");
  p.add<int>("readfix", 'r', "read fixed response", false, 0);
  p.add("version", 'v', "Display version information");

  p.parse_check(argc, const_cast<char**>(argv));

  CHECK(p.get<int>("connections") == 100);
  CHECK(p.get<int>("threads") == 4);
  CHECK(p.get<std::string>("duration") == "10s");
  CHECK(p.get<std::string>("headers") == "HTTPheaders");
  CHECK(p.get<int>("readfix") == 7);
  CHECK(p.exist("version"));
}