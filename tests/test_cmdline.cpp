#include <string>

#include "cmdline/cmdline.h"
#include "doctest/doctest.h"

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

// ./test_gt10 -a A -b B -c C -d D -e E -f F -gh H -i I -j J -k KK
TEST_CASE("test cmd line options more than 10") {
  const char* argv[] = { "test_gt10", "-a", "AA", "-b", "B", "-c", "C", "-d",
                         "D", "-e", "E", "-f", "F", "-g", "G", "-h",
                         "H", "-i", "I", "-j", "J", "-k8"};
  int argc = sizeof(argv) / sizeof(argv[0]);
  cmdline::parser p;

  p.add<std::string>("arg_a", 'a', "argument vector a");
  p.add<std::string>("arg_b", 'b', "argument vector b");
  p.add<std::string>("arg_c", 'c', "argument vector c");
  p.add<std::string>("arg_d", 'd', "argument vector d");
  p.add<std::string>("arg_e", 'e', "argument vector e");
  p.add<std::string>("arg_f", 'f', "argument vector f");
  p.add<std::string>("arg_g", 'g', "argument vector g");
  p.add<std::string>("arg_h", 'h', "argument vector h");
  p.add<std::string>("arg_i", 'i', "argument vector i");
  p.add<std::string>("arg_j", 'j', "argument vector j");
  p.add<int>("arg_k", 'k', "argument vector k", false, 1);

  p.parse_check(argc, const_cast<char**>(argv));

  CHECK(p.get<std::string>("arg_a") == "AA");
  CHECK(p.get<std::string>("arg_b") == "B");
  CHECK(p.get<std::string>("arg_c") == "C");
  CHECK(p.get<std::string>("arg_d") == "D");
  CHECK(p.get<std::string>("arg_e") == "E");
  CHECK(p.get<std::string>("arg_f") == "F");
  CHECK(p.get<std::string>("arg_g") == "G");
  CHECK(p.get<std::string>("arg_h") == "H");
  CHECK(p.get<std::string>("arg_i") == "I");
  CHECK(p.get<std::string>("arg_j") == "J");
  CHECK(p.get<int>("arg_k") == 8);
}

// ./cinatra_press_tool -c100 -t4 -d10s --headers=HTTPheaders -r7
TEST_CASE("test cinatra_press_tool cmd line options without spaces") {
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
TEST_CASE("test cinatra_press_tool cmd line options with spaces") {
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
