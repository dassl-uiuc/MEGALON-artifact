import numpy as np
import sys
import os
import fnmatch
import matplotlib.pyplot as plt

# pass in a list of files / a directory, aggregate all latency files

def calculate_cdf(numbers):
    sorted_numbers = np.sort(numbers)
    n = len(numbers)
    cdf = np.arange(1, n + 1) / n
    return sorted_numbers, cdf

def calculate_p(numbers, p):
    return np.percentile(numbers, p)

def calculate_std_dev(numbers):
    return np.std(numbers)

def get_files_starting_with(directory, prefix):
    files = []
    for file_name in os.listdir(directory):
        if fnmatch.fnmatch(file_name, f"{prefix}*"):
            files.append(os.path.join(directory, file_name))
    return files

def main():
    args = sys.argv[1:]
    avg_lats = []
    ratios = (0.0, 0.25, 0.5, 0.75, 1.0)
    work_dir = args[0]

    a = ("results/limits/hotcachepartial-local/", "results/limits/nomove-hotcachepartial-local/")

    for i in range(2):
        d = a[i]
        avg_lats.append([])
        for ratio in ratios:
            numbers = []
            curr_dir = os.path.join(d, "{:.6f}/12".format(ratio))
            # curr_dir = os.path.join(work_dir, str(ratio), str(i))
            print("curr dir: " + curr_dir)
            file_names = get_files_starting_with(curr_dir, "latencies")
        
            for file_name in file_names:
                try:
                    with open(file_name, 'r') as file:
                        number = [float(line.strip()) for line in file.readlines()]
                        numbers += number[100:]
                except FileNotFoundError:
                    print("File not found. Please make sure the file exists: " + file_name)
                except ValueError:
                    print("Error: The file contains non-numeric data: " + file_name)

            average = np.mean(numbers)
            avg_lats[i].append(average)
            # std_dev = calculate_std_dev(numbers)
            # p999 = calculate_p(numbers, 99.9)
            # p99 = calculate_p(numbers, 99)
            # p50 = calculate_p(numbers, 50)
            # sorted_numbers, cdf = calculate_cdf(numbers)

            # print(f"{average},{std_dev},{p50},{p99},{p999}")

    # for i in range(1, 13):
    #     avg_lats.append([])
    #     for ratio in ratios:
    #         numbers = []
    #         curr_dir = os.path.join(work_dir, "{:.6f}".format(ratio), str(i))
    #         # curr_dir = os.path.join(work_dir, str(ratio), str(i))
    #         print("curr dir: " + curr_dir)
    #         file_names = get_files_starting_with(curr_dir, "latencies")
        
    #         for file_name in file_names:
    #             try:
    #                 with open(file_name, 'r') as file:
    #                     number = [float(line.strip()) for line in file.readlines()]
    #                     numbers += number[100:]
    #             except FileNotFoundError:
    #                 print("File not found. Please make sure the file exists: " + file_name)
    #             except ValueError:
    #                 print("Error: The file contains non-numeric data: " + file_name)

    #         average = np.mean(numbers)
    #         avg_lats[i-1].append(average)
            # std_dev = calculate_std_dev(numbers)
            # p999 = calculate_p(numbers, 99.9)
            # p99 = calculate_p(numbers, 99)
            # p50 = calculate_p(numbers, 50)
            # sorted_numbers, cdf = calculate_cdf(numbers)

            # print(f"{average},{std_dev},{p50},{p99},{p999}")

        # plt.plot(ratios, avg_lats[i-1], label=f'{i}')
        plt.plot(ratios, avg_lats[i], label=d)
        plt.xlabel('shared ratio')
        plt.ylabel('latency (ns)')
        plt.title('Latency vs Shared Cache Ratio')
        # plt.gca().xaxis.set_major_locator(plt.MultipleLocator(1 * 1e6))  # Set tick every 1 unit
        # plt.gca().xaxis.set_minor_locator(plt.MultipleLocator(0.5 * 1e6))
        # plt.xlim(0, p99 * 2)  # Set the x-axis range from 2 to 8
        plt.legend()
        plt.grid(True)
        plt.savefig(f"ratio{i}.png")

    # plt.figure()
    # plt.hist(numbers, bins=100, alpha=0.7, color='blue')
    # plt.xlabel('latency')
    # plt.ylabel('Probability Density')
    # plt.title('Probability Density Function (PDF)')
    # plt.grid(True)
    # plt.savefig("pdf.png")

if __name__ == "__main__":
    main()