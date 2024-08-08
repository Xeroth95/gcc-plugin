void track(int) {}
int untrack(int x) { return x; }

int f() { return 3; };

int main(int argc, char* argv[])
{
	int x = 3;
	track(x);
	{
		int x = argc;
		track(x);
		track(x);
		//track(1);
		if (argc) {
			// comment
			return 1;
		}
		x = f();
		//untrack(untrack(x));
		untrack(x);
	}
	untrack(x);
	return 0;
}
