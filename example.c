#include "track.h"

int f() { return 3; };

int main(int argc, char* argv[])
{
	int x = 3;
	TRACK(x);
	{
		int x = argc;
		// track(x);
		TRACK(x);
		//track(1);
		if (argc) {
			// comment
			//untrack(argc);
			return 1;
		}
		x = f();
		//untrack(untrack(x));
		UNTRACK(x);
	}

	int y = 2;
	for (int i = 0; i < 10; ++i) {
		TRACK(y);
	}
	UNTRACK(y);

	UNTRACK(x);

	{
		int x;
		TRACK(x);
	}

	return 0;
}
