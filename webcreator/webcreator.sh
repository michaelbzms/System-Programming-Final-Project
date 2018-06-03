#!/bin/bash

if [ -e "$1" ] && [ -d "$1" ]; then
	if [ -e "$2" ] && [ -f "$2" ] && [ -r "$2" ]; then
		let linecount=$(wc -l < $2)             # count how many lines in given text_file
		if [ "$linecount" -ge "10000" ]; then
			re='^[0-9]+$'                       # regular expression for integers
			if ! [[ $3 =~ $re ]] || ! [[ $4 =~ $re ]] || [ "$3" -le "0" ] || [ "$4" -le "0" ] ; then
	   			echo "Invalid w or p parameter: not a positive integer number"
			else
				# At this point all script arguements are acceptable
				if ! [ -z "$(ls -A $1)" ]; then                         # if directory is not empty then purge it
					echo "Warning: root directory is full, purging ..."
					rm -rf $1/*
				fi
				let w=$3
				let p=$4
				echo "Starting to create $w web sites with $p html pages on each site ..."
				# First determine each page's name (for all websites)
				declare -a allpages=()
				declare -a page_has_at_least_one_link=()
				let page_counter=0
				let i=0
				while [ "$i" -lt "$w" ]; do                             # this loop is executed $w times
					mkdir $1/site$i                                     # create website_i folder
					page_ids=$(shuf -n $p -i 0-32767)                   # NO repetition is allowed
					for id in $page_ids; do                             # this loop is executed $p times
						page=/site$i/page$i"_"$id.html  
						allpages[$page_counter]=$page                   # add each page name to the list with all the pages
						page_has_at_least_one_link[$page_counter]=false 
						let page_counter=page_counter+1
					done
					let i=i+1
				done
				# Then we create those pages
				let page_counter=0
				previous_site=""
				for page in ${allpages[*]}; do                          # create each html page
					# find out which website this page belongs to
					let site=$(( page_counter / p ))
					if [ "$previous_site" == "" ] || [ "$previous_site" != "$site" ]; then
						echo "Creating web site $site ..."
						previous_site="$site"
					fi
					# step 1: k
					let k=$(( (RANDOM % (linecount - 1998) )  + 2 ))    # 1 < k < linecount-2000  ==  2 <= k <= linecount-1999
					# step 2: m
					let m=$(( (RANDOM % 999 )  + 1001 ))                # 1000 < m < 2000  ==  1001 <= k <= 1999
					echo " Creating page \"$page\" in \"$1\" with $m pages starting from line $k ..."
					# step 3: internal links
					declare -a internal_links=()
					let f=$(((p / 2) + 1))
					echo "  Creating $f internal links for this page ..."
					link_choice=$(shuf -n "$f" -i "$((site * p))-$(((site * p) + p - 1))")   # link choices can NOT be repeated
					let in_link_counter=0
					for choice in $link_choice; do
						if [ "$p" -gt "$f" ] && [ "$choice" -eq "$page_counter" ]; then      # (else) if f == p then we cannot exclude the current page from the internal links 
							let newchoice=$(( ( RANDOM % p ) + (site * p) ))
							while [ "$newchoice" -eq "$page_counter" ] || [[ "${link_choice[*]}" =~ $newchoice ]]; do     # if picked the same page as this or one of the already selected links try again (NOT VERY EFFICIENT)
								let newchoice=$(( ( RANDOM % p ) + (site * p) ))
							done
							let choice=$newchoice
						fi
						internal_links[$in_link_counter]="..${allpages[$choice]}"        # ../sitei/pagei_j.html
						page_has_at_least_one_link[$choice]=true
						let in_link_counter=in_link_counter+1
					done
					# step 4: external links
					declare -a external_links=()
					let q=$(((w / 2) + 1))
					if [ "$w" -eq "1" ]; then                           # if we only have one website then there shall be 0 external links
						let q=0
					else
						if [ "$w" -eq "2" ] && [ "$p" -eq "1" ]; then   # if we have two websites but only one page on each then there shall be only 1 external link
							let q=1
						fi
					fi
					echo "  Creating $q external links for this page ..."
					link_choice=$(shuf -n "$q" -i "0-$((${#allpages[*]} - 1))")             # link choices can NOT be repeated
					let ex_link_counter=0
					for choice in $link_choice; do
						# q is guaranteed to be >1 because w is >1
						if [ "$((choice / p))" -eq "$site" ]; then
							let newchoice=$(( ( RANDOM % ${#allpages[*]} ) ))
							while [ "$((newchoice / p))" -eq "$site" ] || [[ "${link_choice[*]}" =~ $newchoice ]]; do      # if picked a page from the same site or one of the already selected links try again (NOT VERY EFFICIENT)
								let newchoice=$(( ( RANDOM % ${#allpages[*]} ) ))
							done
							let choice=$newchoice
						fi
						external_links[$ex_link_counter]="..${allpages[$choice]}"       # ../sitei/pagei_j.html
						page_has_at_least_one_link[$choice]=true
						let ex_link_counter=ex_link_counter+1
					done
					# step 5: initial headers
					printf "<!DOCTYPE html>\n<html>\n\t<body>\n" > "$1"$page             # overwrite previous contents (which there should not be any since we purged this dir but just in case)
					# step 6:
					let counter=1                                                        # start from 1 because line counters start from 1
					let in_link_counter=0
					let ex_link_counter=0
					while IFS='\n' read -r line && [ "$counter" -lt "$((k+m))" ]; do     # read line-by-line from the file
						if [ "$counter" -ge "$k" ]; then                                 # if line read is between k (and k+m due to the while condition) then print it in the html file
							printf "\t" >> "$1"$page
							if [ "$(((counter - k + 1) % (m/(f+q)) ))" -eq "0" ]; then   # once every "(m/(f+q))" lines put a LINK in that line
								let numOfWords=0
								for word in $line; do                                    # count words in that line
									let numOfWords=numOfWords+1
								done
								let pos=$(( RANDOM % (numOfWords+1) ))                   # random position for the LINK in that line
								let numOfWords=0
								for word in $line; do
									if [ "$numOfWords" -eq "$pos" ]; then
										# if we are out of external links OR we have internal links and 50% chance favours us then pick an internal link
										# else pick an external link (which is guaranted to exist)
										link=""
										if [ "$ex_link_counter" -ge "${#external_links[*]}" ] || ( [ "$in_link_counter" -lt "${#internal_links[*]}" ] && [ "$((RANDOM % 2))" -eq "0" ] ); then    # 50% chance we go for internal link
											link=${internal_links[$in_link_counter]}
											let in_link_counter=in_link_counter+1
										else                                             # else use an external link
											link=${external_links[$ex_link_counter]}
											let ex_link_counter=ex_link_counter+1
										fi
										echo "  Adding link to $link ..."
										printf "<a href=\"""$link""\">""$link""</a> " >> "$1"$page
									fi
									modified_word=$(printf "$word" | tr -d "\r\n")       # delete both \r and \n from the last word of the sentence
									printf "$modified_word " >> "$1"$page
									let numOfWords=numOfWords+1
								done
								if [ "$numOfWords" -eq "$pos" ]; then
									# if we are out of external links OR we have internal links and 50% chance favours us then pick an internal link
									# else pick an external link (which is guaranted to exist)
									link=""
									if [ "$ex_link_counter" -ge "${#external_links[*]}" ] || ( [ "$in_link_counter" -lt "${#internal_links[*]}" ] && [ "$((RANDOM % 2))" -eq "0" ] ); then    # 50% chance we go for internal link
										link=${internal_links[$in_link_counter]}
										let in_link_counter=in_link_counter+1
									else                                                 # else use an external link
										link=${external_links[$ex_link_counter]}
										let ex_link_counter=ex_link_counter+1
									fi
									echo "  Adding link to $link ..."
									printf "<a href=\"""$link""\">""$link""</a> " >> "$1"$page
								fi
								printf "<br>\n" >> "$1"$page
							else        # else print line as is
								modified_line=$(printf "$line" | tr -d '\r')             # delete all '\r'
								printf "$modified_line""<br>\n" >> "$1"$page             # add <br>\n
							fi
						fi
						let counter=counter+1
					done < $2    # read lines from text_file
					printf "\t</body>\n</html>\n" >> "$1"$page                           # append final headers
					let page_counter=page_counter+1
				done
				# check if all pages have at least one incoming link:
				let page_counter=0
				found_false=false
				while [ "$page_counter" -lt "${#allpages[*]}" ]; do
					if [ "${page_has_at_least_one_link[$page_counter]}" = false ]; then
						echo "Waring: page \"$1${allpages[$page_counter]}\" has no incoming links"
						found_false=true
					fi
					let page_counter=page_counter+1
				done
				if [ "$found_false" = false ]; then
					echo "All pages have at least one incoming link"
				fi
				echo "Done"
			fi
		else
			echo "text_file given does not contain at least 10.000 lines"
		fi
	else
		echo "Invalid text_file parameter or no read permission"
	fi
else
	echo "Invalid root_directory parameter"
fi
