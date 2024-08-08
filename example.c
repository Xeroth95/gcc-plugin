void track(int) {}
int untrack(int x) { return x; }

int f() { return 3; };

int main(int argc, char* argv[])
{
	int x = 3;
	track(x);
	{
		int x = argc;
		// track(x);
		track(x);
		//track(1);
		if (argc) {
			// comment
			//untrack(argc);
			return 1;
		}
		x = f();
		//untrack(untrack(x));
		untrack(x);
	}

	for (int i = 0; i < 10; ++i) {
		track(i);
	}

	untrack(x);
	return 0;
}
