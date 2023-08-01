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
    
    return numbers

def compute_mean(numbers):
    if not numbers:
        return None
    return sum(numbers) / len(numbers)

def compute_statistics(numbers):
    if not numbers:
        return None, None, None
    mean = compute_mean(numbers)
    minimum = min(numbers)
    maximum = max(numbers)
    return mean, minimum, maximum

# Example usage:
file_path = input("Please enter the path of the file: ")
numbers_list = read_numbers_from_file(file_path)

if numbers_list:
    mean, minimum, maximum = compute_statistics(numbers_list)
    #print("Numbers:", numbers_list)
    print("Number of measures", len(numbers_list))
    print("Mean:", mean)
    print("Minimum:", minimum)
    print("Maximum:", maximum)
else:
    print("No numbers found in the file.")