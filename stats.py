def read_numbers_from_file(file_path):
    numbers = []
    try:
        with open(file_path, 'r') as file:
            for line in file:
                try:
                    number = float(line.strip())
                    numbers.append(number)
                except ValueError:
                    print("Warning: Could not convert '{line.strip()}' to a number. Skipping this line.")
    except FileNotFoundError:
        print("Error: The file was not found.")
        return 0
    
    return numbers

def compute_mean(numbers):
    if not numbers:
        return None
    return sum(numbers) / len(numbers)

def compute_variance(numbers):
    if not numbers:
        return None
    mean = compute_mean(numbers)
    squared_diff_sum = sum((x - mean) ** 2 for x in numbers)
    variance = squared_diff_sum / len(numbers)
    return variance

def compute_statistics(numbers):
    if not numbers:
        return None, None, None, None
    mean = compute_mean(numbers)
    variance = compute_variance(numbers)
    minimum = min(numbers)
    maximum = max(numbers)
    return mean, variance, minimum, maximum

# Example usage:
file_path = raw_input("Please enter the path of the file: ")
numbers_list = read_numbers_from_file(file_path)

if numbers_list:
    mean, variance, minimum, maximum = compute_statistics(numbers_list)
    print("Numbers:", numbers_list)
    print("Mean:", mean)
    print("Variance:", variance)
    print("Minimum:", minimum)
    print("Maximum:", maximum)
else:
    print("No numbers found in the file.")