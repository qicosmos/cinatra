#include "unit_test.hpp"
#include <csignal>

[[noreturn]]
static void report_and_exit()
{
	std::cout << "\n**** ";
	std::cout << UnitTest::getInstance().getFailureNum() << " failures are detected." << std::endl;
	exit((int)UnitTest::getInstance().getFailureNum());
}

int main()
{
	signal(SIGSEGV, [](int)
	{
		UnitTest::getInstance().incFailure();
		std::cout << ">>> fatal error: received SIGSEGV." << std::endl;
		UnitTest::getInstance().printLastCheckedPoint();
		report_and_exit();
	});
	UnitTest::getInstance().runAll();
	report_and_exit();
}
