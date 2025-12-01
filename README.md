SYSC4001_A3P2

## Overview
This project implements a concurrent solution for n number of TA marking exams using shared memory and semaphores. 

## Features
- Uses shared memory to access 
- Synchronizes concurrent accesses with semaphores.
- Implements readers-writers pattern for the exam rubric.
- Supports multiple TA processes marking exams simultaneously.
- Progresses through multiple exam files until termination exam `9999` is reached.
- Logs TA activities by prinitng status updates.

## Compile
- gcc -std=c99 -Wall -lrt -pthread -o marker TA.c
- ./marker `Number of TA(0,1..10)` `with or without semaphore 1(part b)/0(part a)`
