import time

from selenium import webdriver
from selenium.webdriver.common.by import By

driver = webdriver.Chrome()
driver.get("https://www.nytimes.com/puzzles/spelling-bee")
driver.set_window_size(1680, 939)
time.sleep(15)  # Sleep for 10 seconds

# Find out what letter is at each cell location, build a string
todays_letters = ""
for i in range(1, 8):
    todays_letters += driver.find_element(By.CSS_SELECTOR, ".hive-cell:nth-child(" + str(i) + ")").text

f = open("spellingbee_filename", "r")
for test_word in f:
    test_word = test_word.strip().upper()
    for letter_as_num_position in range(0, len(test_word)):
        # A T T I C
        position = todays_letters.find(test_word[letter_as_num_position])
        driver.find_element(By.CSS_SELECTOR, ".hive-cell:nth-child(" + str(position + 1) + ") > .cell-fill").click()
    driver.find_element(By.CSS_SELECTOR, ".hive-action__submit").click()
f.close()