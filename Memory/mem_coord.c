#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <libvirt/libvirt.h>


#define MEM_STARVE_THRESHOLD (150 * 1024)
#define MEM_WASTE_THRESHOLD  (300 * 1024)


typedef struct DomainMemInfo
{
  virDomainPtr       domain;
  unsigned long long memory;
  
}DomainMemInfo_t;


typedef struct DomainInfo
{
  int          domain_id;
  char         *domain_name;
}DomainInfo_t;


typedef struct AllDomainsList 
{
  int          count;         /* number of domains in the array */
  virDomainPtr *domains; /* pointer to array of Libvirt domains */
  DomainInfo_t *domain_info;
}AllDomainsList_t;


AllDomainsList_t myos_get_domains_list(virConnectPtr conn)
{
  virDomainPtr     *domains;
  int              num_domains, domain_counter, name_len;
  int              *Domain_ids;
  AllDomainsList_t *list = NULL;

  unsigned int flags = (VIR_CONNECT_LIST_DOMAINS_ACTIVE |
		       VIR_CONNECT_LIST_DOMAINS_RUNNING);

  num_domains = virConnectListAllDomains(conn, &domains, flags);

  if(num_domains > 0)
  {
    list = malloc(sizeof(AllDomainsList_t));
    list->count = num_domains;
    list->domain_info = calloc(num_domains, sizeof(DomainInfo_t));
    list->domains     = domains;
    /*member of domain_info will be re-freshed every X second (X=argv[1]) */

    /*List Active VM*/
    Domain_ids = malloc(sizeof(int) * num_domains);
    (void)virConnectListDomains(conn, Domain_ids, num_domains);

    #ifdef DEBUG_ENA
    printf("----- Write Domain List -----\n");
    #endif

    /*initialize domain_info for each domain*/
    for (domain_counter = 0; domain_counter < num_domains; domain_counter++) 
    {

     list->domain_info[domain_counter].domain_id = Domain_ids[domain_counter];

     name_len  = strlen(virDomainGetName(domains[domain_counter]));

     list->domain_info[domain_counter].domain_name = calloc(name_len, sizeof(char));

     strncpy(list->domain_info[domain_counter].domain_name, virDomainGetName(domains[domain_counter]), name_len);

    #ifdef DEBUG_ENA
     printf("ID:%d  %8s\n", list->domain_info[domain_counter].domain_id, list->domain_info[domain_counter].domain_name);
    #endif
    }
    free(Domain_ids);

  }
  else
  {
    fprintf(stderr, "No active domain, exit now...\n");
    exit(1);
  }

  return *list;

}


void myos_vcpu_scheduler_preprocess
(
  char           *cmd_line_param,

  int            *timer
)
{
  if(cmd_line_param == NULL)
  {
    fprintf(stderr, "No input argument, please execute in below way:\n");
    fprintf(stderr, "./vcpu_scheduler <input_argument>\n");
    fprintf(stderr, "exit...\n");
    exit(1);
  }

  *timer = atoi(cmd_line_param);

  if(*timer == 0)
  {
    fprintf(stderr, "invalid input argument = 0, please specify a non-zero number\n");
    fprintf(stderr, "exit...\n");
    exit(1);
  }

  fprintf(stderr, "CPU scheduler START...\n");
  fprintf(stderr, "User set refresh timer as %d \n", *timer);

}


void myos_open_local_connection
(
  virConnectPtr  *conn
)
{
  *conn = virConnectOpen("qemu:///system");
  if (*conn == NULL) 
  {
    fprintf(stderr, "Failed to open connection to qemu:///system\n");
    exit(1);
  }
  else
  {
    fprintf(stderr, "Successful connection to qemu:///system\n");
  }

}


int main(int argc, char *argv[])
{
  virConnectPtr conn;
  AllDomainsList_t      all_domain_list;
  int numActiveDomains, domain_counter, timer;
  char *hostname;

  unsigned long long  memory_adjust;

  /*****  Pre-process the cmd line input   *****/
  myos_vcpu_scheduler_preprocess(argv[1], &timer);

  /*****  Open local connection   *****/
  myos_open_local_connection(&conn);

  hostname = virConnectGetHostname(conn);

  if(hostname == NULL)
  {
    fprintf(stderr, "Failed to get the Host info\n");
  }
  else
  {  
    printf("----- Host Info -----\n");
    fprintf(stdout, "Hostname:%s\n\n\n", hostname);
    free(hostname);
  }


  while((numActiveDomains = (all_domain_list = myos_get_domains_list(conn)).count) > 0) 
  {

    DomainMemInfo_t  max_mem_domain, min_mem_domain;

    max_mem_domain.memory = 0;
    min_mem_domain.memory = 0;

    for (domain_counter = 0; domain_counter < numActiveDomains; domain_counter++) 
    {
      virDomainMemoryStatStruct memstats[VIR_DOMAIN_MEMORY_STAT_NR];

      if(virDomainSetMemoryStatsPeriod(all_domain_list.domains[domain_counter],1,VIR_DOMAIN_AFFECT_CURRENT) != -1)
      {
        if(virDomainMemoryStats(all_domain_list.domains[domain_counter], memstats,VIR_DOMAIN_MEMORY_STAT_NR, 0) != -1)
        {
	  printf("Domain %s available memory: %llu MB\n",
	           virDomainGetName(all_domain_list.domains[domain_counter]),
		       (memstats[VIR_DOMAIN_MEMORY_STAT_AVAILABLE].val)/1024);

	  if (memstats[VIR_DOMAIN_MEMORY_STAT_AVAILABLE].val > max_mem_domain.memory) 
          {
	    max_mem_domain.domain = all_domain_list.domains[domain_counter];
	    max_mem_domain.memory = memstats[VIR_DOMAIN_MEMORY_STAT_AVAILABLE].val;
	  }

	  if ((min_mem_domain.memory == 0) ||
                (memstats[VIR_DOMAIN_MEMORY_STAT_AVAILABLE].val < min_mem_domain.memory)) 
          {
	    min_mem_domain.domain = all_domain_list.domains[domain_counter];
	    min_mem_domain.memory = memstats[VIR_DOMAIN_MEMORY_STAT_AVAILABLE].val;
	  }
        }
        else
        {
          fprintf(stderr, "Failed to get memory status for domain %8s, exit...\n", 
                                       all_domain_list.domain_info[domain_counter].domain_name);
          exit(1);
        }

      }
      else
      {
        fprintf(stderr, "Failed to set balloon driver stat collection period for domain %8s, exit...\n", 
                                       all_domain_list.domain_info[domain_counter].domain_name);
        exit(1);
      }
    }//domain loop


    /*Now perform memory re-allocation if needed*/
    if(min_mem_domain.memory <= (unsigned long long)MEM_STARVE_THRESHOLD) 
    {
      if(max_mem_domain.memory >= (unsigned long long)MEM_WASTE_THRESHOLD) 
      {
        /*Case 1: min memory domain is starving && max memory domain is wasting*/
        /*Solution: take out memory from wasting domain and allocate it to starving domain*/
        memory_adjust = (max_mem_domain.memory/2);

	virDomainSetMemory(max_mem_domain.domain,  (max_mem_domain.memory - memory_adjust));
	printf("A1) Reduce %llu MB from wasteful domain %8s\n", (memory_adjust/1024), virDomainGetName(max_mem_domain.domain));

	virDomainSetMemory(min_mem_domain.domain,  (min_mem_domain.memory + memory_adjust));
        printf("A2) Adding %llu MB to starved domain %8s\n", (memory_adjust/1024), virDomainGetName(min_mem_domain.domain));
      }
      else 
      {
        /*Case 2: min memory domain is starving, max memory domain is NOT wasting*/
        /*Solution: just add memory to starving domain only*/
        memory_adjust = MEM_WASTE_THRESHOLD;

        virDomainSetMemory(min_mem_domain.domain, (min_mem_domain.memory + memory_adjust));
        printf("B1) Adding %llu MB to starved domain %s\n only", (memory_adjust/1024), virDomainGetName(min_mem_domain.domain));
      }
    } 
    else if(max_mem_domain.memory >= MEM_WASTE_THRESHOLD)
    {
      /*Case 3: max memory domain is wasting, min memory domain is NOT starving*/
      /*Solution: just reduce memory from wasting domain only*/
      memory_adjust = (MEM_STARVE_THRESHOLD/2);

      virDomainSetMemory(max_mem_domain.domain, (max_mem_domain.memory - memory_adjust));
      printf("C1) Reduce %llu MB from wasteful domain %s\n only", (memory_adjust/1024), virDomainGetName(max_mem_domain.domain));
    }



    for (domain_counter = 0; domain_counter < numActiveDomains; domain_counter++) 
    {
      /*Domain loop clean-up*/
      all_domain_list.domain_info[domain_counter].domain_id = 0;
      free(all_domain_list.domain_info[domain_counter].domain_name);
      virDomainFree(all_domain_list.domains[domain_counter]);
        
    }//domain loop


    free(all_domain_list.domain_info);
    all_domain_list.count = 0;
    printf("\n\n");
    sleep(timer);
  }//end while
  
  virConnectClose(conn);
  free(conn);

  return 0;
}


