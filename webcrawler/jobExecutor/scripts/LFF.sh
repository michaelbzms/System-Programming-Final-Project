#!/bin/bash

if [ -e "../log" ] && [ -d "../log" ] && ! [ -z "$(ls -A ../log)"  ]; then       # if directory exists and is not empty
	# First, find all successfully searched words
	declare -a wordlist        # bash array containing each word found suitable only once
	for file in ../log/*; do   # for each file in ../log/ folder
		while IFS=':' read -r timestamp operation keyword paths; do
			# if operation is "search" and the keyword was not found before and the search was successful
			if [ $operation == "search" ] && ! [[ "${wordlist[*]}" =~ $keyword ]] && ! [[ "${paths[*]}" =~ " -" ]]; then 
				wordlist+=$keyword
			fi
		done < $file           # redirect input to come from each file in ../log/
	done
	# Now, use that wordlist to find all lines containing each word using grep
	# for each word we count how many paths it was found on and in the end keep the word with the maximum count
	let min=9999999
	minword=""
	for word in ${wordlist[*]}; do
		declare -a pathlist=()      # init empty pathlist
		let count=0
		searchword="search : $word"
		lines=$(grep "$searchword" ../log/*)
		while IFS=':' read filepath timestamp operation keyword paths 
			do
				for path in $paths; do
					if [ $path != - ] && ! [[ "${pathlist[*]}" =~ $path ]]; then        # if path is not "-" and not counted before
						pathlist+=$path
						let count=count+1
					fi
				done
			done <<< "$lines"
		if [ "$count" -lt "$min" ]; then
			let min=count
			minword=$word
		fi
	done
	if [ "$min" -ne "9999999" ]; then
		echo "Keyword least frequently found:" $minword "[TotalNumFilesFound:" $min "]"
	else
		echo "There are no words successfully searched"
	fi
else
	echo "could not find log folder or is empty"
fi
