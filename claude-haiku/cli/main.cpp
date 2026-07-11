#include "CliRunner.h"

int main(int argc, char* argv[])
{
	const cli::Options opts = cli::ParseArgs(argc, argv);

	if (opts.mode == cli::Mode::Interactive)
		return cli::RunInteractive(opts);

	return cli::RunOneShot(opts);
}
