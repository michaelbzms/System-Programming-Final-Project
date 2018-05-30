#!/bin/bash

if [ -e "../log" ] && [ -d "../log" ] && ! [ -z "$(ls -A ../log)"  ]; then   # if directory exists and is not empty
	let count=0
	declare -a wordlist         # bash array containing each word found suitable only once
	for file in ../log/*; do    # for each file in ../log/
		while IFS=':' read -r timestamp operation keyword paths; do
			# if operation is "search" and the keyword was not counted/found before and the search was successful
			if [ $operation == "search" ] && ! [[ "${wordlist[*]}" =~ $keyword ]] && ! [[ "${paths[*]}" =~ " -" ]]; then 
				wordlist+=$keyword
				let count=count+1
			fi
		done < $file            # redirect input to come from each file in ../log/
	done
	echo "Total number of keywords (successfully) searched:" $count
	echo "Those words are:" ${wordlist[*]}
else
	echo "Could not find log folder or is empty"
fi
