/* void track(int) {} */
/* int untrack(int x) { return x; } */

int main(int argc, char* argv[])
{
	int y = 2;

	f(3);
	//int z = f(3);

	/* track(y); */
	/* for (int i = 0; i < 10; ++i) { */
	/* 	untrack(y); */
	/* } */

	{
		int y;
		track(y);

		if (argc) {
//			untrack(y);
			return 1;
		}

		int x = 4;

		untrack(y);
	}

	return y;
}
