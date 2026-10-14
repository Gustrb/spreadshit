import os
import subprocess
from os.path import isfile, join

BASE_TEST_DIR="tests"

def main():
    outfiles = []
    inputfiles = []
    for f in os.listdir(BASE_TEST_DIR):
        if ".out" in f:
            outfiles.append(join(BASE_TEST_DIR, f))
        else:
            inputfiles.append(join(BASE_TEST_DIR, f))

    if len(outfiles) != len(inputfiles):
        print("Some files are missing, every .csv file must have a matching .out.csv file")
        return

    outfiles.sort()
    inputfiles.sort()

    for i in range(len(outfiles)):
        run_test(outfiles[i], inputfiles[i])

def run_test(outfilepath, inputfilepath):
    print(f"Running {inputfilepath}")

    with open(inputfilepath) as infile:
        result = subprocess.run(["./main"], stdin=infile, capture_output=True, check=True).stdout.decode("utf-8")

    with open(outfilepath) as outfile:
        outcontents = outfile.read()

    if result == outcontents:
        print("Pass...")
    else:
        print(f"Fail!\nExpected: \n{outcontents}")
        print(f"Got: \n{result}")


if __name__ == "__main__":
    main()
